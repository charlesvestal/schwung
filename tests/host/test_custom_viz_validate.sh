#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# AN UNKNOWN CUSTOM KIND IS LEGAL. A CUSTOM KIND WITH NO SCRIPT IS A MISTAKE.
#
# The first is how an OLDER HOST reads a NEWER module. It must not be an error:
# the module is correct and the host is simply older, and flagging it would make
# every forward-compatible module look broken.
#
# The second is an author who declared a widget and forgot to ship the file.
# Nothing else in the system will ever tell them -- the widget just silently
# never appears, because an unregistered kind falls through to the detector and
# the built-in draws a perfectly reasonable picture in its place.
#
# THE CHECK ONLY FIRES WHEN capabilities IS SUPPLIED. No existing caller passes
# it (audit_sheet, validate.mjs and the fleet fixture tests all omit it), so a
# contract validated without capabilities cannot suddenly grow a warning it has
# no way to answer.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the validator tests" >&2
  exit 1
fi

node --input-type=module -e '
import { validateContract } from "./src/shared/param_pages/validate_contract.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const run = (chainParams, capabilities) => validateContract({
  id: "t",
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: chainParams.map((p) => p.key),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams, capabilities,
}).findings;

const CUSTOM = [{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
                  viz: { kind: "custom:meter" } }];
const BUILTIN = [{ key: "drive", name: "Drive", type: "float", min: 0, max: 1,
                   viz: { kind: "fader" } }];
const hasRule = (f, r) => f.some((x) => x.rule === r);
const errs = (f) => f.filter((x) => x.level === "error");

/* Declared, ships a script: nothing to say. */
let f = run(CUSTOM, { canvas_script: "canvas.js" });
ok(!hasRule(f, "custom-widget-no-script"),
   "a custom kind with a canvas_script is not warned about");
ok(errs(f).length === 0, "and produces no errors");

/* Declared, no script: the one mistake worth reporting. */
f = run(CUSTOM, {});
ok(hasRule(f, "custom-widget-no-script"),
   "a custom kind with no canvas_script is warned about");
ok(f.find((x) => x.rule === "custom-widget-no-script").level === "warn",
   "it is a WARNING, not an error -- the module still renders");
ok(/custom:meter/.test(f.find((x) => x.rule === "custom-widget-no-script").message),
   "the warning names the kind that will not load");

/* NOT SUPPLIED: silent, because the caller cannot answer the question. */
f = run(CUSTOM, undefined);
ok(!hasRule(f, "custom-widget-no-script"),
   "with no capabilities supplied the check does not fire");

/* A built-in kind is untouched by any of this. */
f = run(BUILTIN, {});
ok(!hasRule(f, "custom-widget-no-script"),
   "a built-in viz kind produces no custom-widget warning");

/* A grouped custom kind is caught too -- the kind may sit on any member. */
f = run([{ key: "x", name: "X", type: "float", min: 0, max: 1,
           viz: { group: "pad", role: "x", kind: "custom:xy" } },
         { key: "y", name: "Y", type: "float", min: 0, max: 1,
           viz: { group: "pad", role: "y" } }], {});
ok(hasRule(f, "custom-widget-no-script"),
   "a custom kind declared on a GROUP is warned about too");

process.exit(fail ? 1 : 0);
'
