// Repro for the enum-display parseInt bug. formatMetaOptionValue runs
// parseInt(rawValue, 10) which stops at the first non-digit, so fraction
// strings like "1/8" parse to 1 — and the function returns options[1]
// regardless of the actual value. Symptom: arp/SuperArp Rate stuck at
// default-looking value, tapedelay division alternates between "free" / "1/1".
//
// RED today, GREEN after the fix in src/shadow/shadow_ui.js.

import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const source = readFileSync(path.join(repoRoot, 'src/shadow/shadow_ui.js'), 'utf8');

function extractFn(name) {
    const re = new RegExp(`(^|\\n)function\\s+${name}\\s*\\(`);
    const m = re.exec(source);
    if (!m) throw new Error(`function ${name} not found`);
    const start = m.index + (m[1] ? 1 : 0);
    let i = source.indexOf('{', start);
    let depth = 1, pos = i + 1;
    while (depth > 0 && pos < source.length) {
        const c = source[pos++];
        if (c === '{') depth++;
        else if (c === '}') depth--;
    }
    return source.slice(start, pos);
}

const sandbox = { console };
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(extractFn('formatMetaOptionValue'), sandbox);

const arpOptions     = ["1/4.","1/4","1/4T","1/8.","1/8","1/8T","1/16.","1/16","1/16T","1/32"];
const superArpOptions = ["1/32","1/16","1/8","1/4"];
const tapeDelayOpts  = ["free","1/1","1/2","1/2d","1/4","1/4d","1/4t","1/8","1/8d","1/8t","1/16","1/16t"];

const failures = [];
function check(label, got, want) {
    const ok = got === want;
    console.log(`  ${ok ? "ok  " : "FAIL"} - ${label}  got=${JSON.stringify(got)} want=${JSON.stringify(want)}`);
    if (!ok) failures.push(label);
}

const fmt = sandbox.formatMetaOptionValue;

// --- option-label rawValue (the buggy case) -----------------------------
console.log("Option-label rawValue should be returned as-is:");
check("arp 1/8",       fmt({ options: arpOptions     }, "1/8"),  "1/8");
check("arp 1/16",      fmt({ options: arpOptions     }, "1/16"), "1/16");
check("arp 1/32",      fmt({ options: arpOptions     }, "1/32"), "1/32");
check("superarp 1/4",  fmt({ options: superArpOptions }, "1/4"),  "1/4");
check("superarp 1/8",  fmt({ options: superArpOptions }, "1/8"),  "1/8");
check("tapedelay 1/2", fmt({ options: tapeDelayOpts  }, "1/2"),  "1/2");
check("tapedelay 1/8", fmt({ options: tapeDelayOpts  }, "1/8"),  "1/8");
check("tapedelay 1/16t", fmt({ options: tapeDelayOpts }, "1/16t"), "1/16t");
check("tapedelay free",  fmt({ options: tapeDelayOpts }, "free"),  "free");

// --- numeric-index rawValue (must still work) ----------------------------
console.log("Numeric-index rawValue should still resolve to the option:");
check("arp idx 7 → 1/16",    fmt({ options: arpOptions     }, "7"), "1/16");
check("superarp idx 0 → 1/32", fmt({ options: superArpOptions }, "0"), "1/32");
check("tapedelay idx 0 → free", fmt({ options: tapeDelayOpts }, "0"), "free");
check("tapedelay idx 11 → 1/16t", fmt({ options: tapeDelayOpts }, "11"), "1/16t");

// --- option_labels map should still take priority -----------------------
console.log("option_labels map still takes priority:");
const meta = { options: ["a","b","c"], option_labels: { "0": "Alpha", "1": "Beta", "2": "Gamma" } };
check("option_labels lookup 1 → Beta", fmt(meta, "1"), "Beta");

// --- out-of-range / non-numeric falls through to rawValue ---------------
console.log("Out-of-range / unknown values pass through:");
check("unknown rawValue", fmt({ options: arpOptions }, "nope"), "nope");

if (failures.length) { console.log(`\nFAIL: ${failures.length} assertion(s) failed`); process.exit(1); }
console.log("\nPASS"); process.exit(0);
