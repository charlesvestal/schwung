// A module may publish ui_hierarchy for the BROWSER without surrendering its
// own on-device editor.
//
// The two surfaces read the same key and mean different things by it.
// schwung-manager builds the Remote UI's control list from ui_hierarchy and
// has no other source — no hierarchy, no controls, just "No parameters
// available". On the device, publishing it diverts enterComponentEdit() to the
// generic hierarchy editor and the module's own ui_chain.js never loads.
//
// So a module shipping a real chain UI had to pick one surface. work and smack
// both picked the device and have no Remote UI at all. "remote_only" lets a
// hierarchy address the browser only.
//
// Guards the predicate AND that every device-side divert actually consults it —
// a helper nothing calls would pass a behaviour test and change nothing.

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
vm.runInContext(extractFn('hierarchyDrivesDeviceEditor'), sandbox);
const drives = sandbox.hierarchyDrivesDeviceEditor;

let failed = 0;
function check(cond, what) {
    if (cond) return;
    console.error(`FAIL: ${what}`);
    failed++;
}

const levels = { levels: { root: { label: 'X', params: ['a'] } } };

/* The behaviour that already existed must not change: a plain hierarchy still
 * claims the device editor, and no hierarchy still falls through. */
check(drives(levels) === true, 'an ordinary hierarchy still drives the device editor');
check(drives(null) === false, 'a missing hierarchy does not drive the device editor');
check(drives(undefined) === false, 'undefined does not drive the device editor');

/* The new behaviour. */
check(drives({ ...levels, remote_only: true }) === false,
      'remote_only: true releases the device editor');

/* Only the literal true. A module that sends "true", 1, or a stray non-empty
 * string should NOT silently lose its device editor — the failure mode of a
 * loose check is a module that ships fine and then has no UI on hardware,
 * which is exactly the bug this flag exists to avoid causing. */
for (const loose of ['true', 1, 'yes', {}]) {
    check(drives({ ...levels, remote_only: loose }) === true,
          `remote_only: ${JSON.stringify(loose)} is not the literal true, so the device editor stays`);
}
check(drives({ ...levels, remote_only: false }) === true, 'remote_only: false keeps the device editor');

/* Every place a hierarchy takes over a device screen must ask. Counting the
 * call sites is the part that would rot silently: a new divert added later
 * would reintroduce the fork for remote_only modules. */
const callSites = (source.match(/hierarchyDrivesDeviceEditor\(/g) || []).length;
check(callSites >= 5,
      `expected the predicate at its definition plus every divert site, found ${callSites}`);

/* And no divert may still test the hierarchy's raw truthiness. */
const rawDiverts = [
    /if \(hierarchy\) \{\s*\n\s*debugLog\(`enterComponentEdit/,
    /const hierarchy = getMasterFxHierarchy\(selectedMasterFxComponent\);\s*\n\s*if \(hierarchy\) \{/,
];
for (const re of rawDiverts) {
    check(!re.test(source),
          `a device divert still branches on the raw hierarchy: ${re}`);
}

if (failed) {
    console.error(`${failed} failed`);
    process.exit(1);
}
console.log('PASS: remote_only hierarchies address the browser without claiming the device editor');
