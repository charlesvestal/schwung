#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The two trailing pages ("My Presets" / "Module") reach a module that draws
# its OWN param pages.
#
# Every component the shadow UI paginates gets them, because enterParamPages
# hands componentParamPagesIo to the controller and the controller appends
# whatever io.trailingMenus returns (test_trailing_pages_wiring.sh pins that).
# A module shipping ui_chain.js builds its own controller, so it got neither —
# a drum machine with a pad-select editor could not save a preset while a synth
# on the stock editor could. The pages are not a property of who drew the grid.
#
# Behaviour over structure, in this suite's house style: setupModuleParamShims
# and clearModuleParamShims are LIFTED out of shadow_ui.js with `new Function`
# and actually RUN against stubs, because a grep for the binding can pass while
# the code underneath never executes. Each lift ends on a marker that only
# appears if the pasted body ran.

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "  ok  $*"; }

UI="src/shadow/shadow_ui.js"
API="docs/API.md"

[ -f "$UI" ] || fail "missing $UI"
command -v node >/dev/null 2>&1 || fail "node is required"

# ---------------------------------------------------------------------------
# 1. BEHAVIOUR: the bindings install, carry slot+component, and clear.
# ---------------------------------------------------------------------------

node - "$UI" <<'NODE' || fail "chain-UI trailing bindings do not behave"
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");

function lift(name) {
    const i = src.indexOf(`function ${name}(`);
    if (i < 0) throw new Error(`${name} not found`);
    /* brace-match the declaration */
    let d = 0, started = false, end = i;
    for (let p = i; p < src.length; p++) {
        const c = src[p];
        if (c === "{") { d++; started = true; }
        else if (c === "}") { d--; if (started && d === 0) { end = p + 1; break; } }
    }
    return src.slice(i, end);
}

const calls = [];
const g = {};
/* Only what the two lifted functions actually touch. If either grows a new
 * dependency this throws, which is the point: the test breaks loudly rather
 * than measuring a stub. */
const env = {
    globalThis: g,
    paramShimsInstalled: false,
    originalHostGetParam: undefined,
    originalHostSetParam: undefined,
    getComponentParamPrefix: (k) => (k === "synth" ? "synth" : k),
    getSlotParam: () => "",
    setSlotParam: () => true,
    slotChainComponentIndex: () => 0,
    unloadModuleUi: () => {},
    enterComponentSelect: () => {},
    componentTrailingMenus: (slot, comp, prefix) => {
        calls.push(["menus", slot, comp, prefix]);
        return [{ name: "My Presets", entries: [{ label: "Preset" }] },
                { name: "Module", entries: [{ label: "Swap Module" }] }];
    },
    runComponentActionFromGrid: (slot, comp, action) => {
        calls.push(["action", slot, comp, action]);
        return action === "up_load";      /* pretend Load opened a screen */
    },
    scanForToolModules: () => [],
    toolModules: [],
};

const names = Object.keys(env);
const body = lift("setupModuleParamShims") + "\n" + lift("clearModuleParamShims") +
             "\nsetupModuleParamShims(2, 'synth');\nreturn 'ran';";
const marker = new Function(...names, body)(...names.map((n) => env[n]));
if (marker !== "ran") throw new Error("lifted body did not execute");

if (typeof g.shadow_component_trailing_menus !== "function")
    throw new Error("shadow_component_trailing_menus was not installed");
if (typeof g.shadow_component_run_action !== "function")
    throw new Error("shadow_component_run_action was not installed");

const menus = g.shadow_component_trailing_menus();
if (!Array.isArray(menus) || menus.length !== 2)
    throw new Error("trailing menus did not come through");
if (menus[0].name !== "My Presets" || menus[1].name !== "Module")
    throw new Error("wrong pages: " + menus.map((m) => m.name).join(","));
/* Bound with the slot and component already applied — the module never sees
 * them, exactly as host_swap_module works. */
const m = calls.find((c) => c[0] === "menus");
if (m[1] !== 2 || m[2] !== "synth")
    throw new Error("menus not bound to the slot/component: " + JSON.stringify(m));

if (g.shadow_component_run_action("up_save") !== false)
    throw new Error("an action that opens nothing must report false");
if (g.shadow_component_run_action("up_load") !== true)
    throw new Error("an action that opens a screen must report true");
if (g.shadow_component_run_action("") !== false)
    throw new Error("an empty action must be inert");
const a = calls.find((c) => c[0] === "action");
if (a[1] !== 2 || a[2] !== "synth")
    throw new Error("action not bound to the slot/component: " + JSON.stringify(a));

/* And they must not outlive the module that owned them. */
const clear = new Function(...names, lift("clearModuleParamShims") +
                           "\nclearModuleParamShims();\nreturn 'ran';");
if (clear(...names.map((n) => env[n])) !== "ran")
    throw new Error("clear body did not execute");
if (g.shadow_component_trailing_menus !== undefined ||
    g.shadow_component_run_action !== undefined)
    throw new Error("bindings survived clearModuleParamShims");
NODE
pass "bindings install, carry slot+component, report screen-opening, and clear"

# ---------------------------------------------------------------------------
# 2. The host's own path is untouched — it still goes through the one helper.
# ---------------------------------------------------------------------------

grep -q "trailingMenus: () => componentTrailingMenus(slotIndex, componentKey, prefix)" "$UI" \
    || fail "componentParamPagesIo no longer supplies trailingMenus"
pass "the shadow UI's own trailing-menu wiring is unchanged"

# ---------------------------------------------------------------------------
# 3. Documented, with the guard a module needs on an older host.
# ---------------------------------------------------------------------------

grep -q "shadow_component_trailing_menus" "$API" || fail "$API does not document the binding"
grep -q "shadow_component_run_action"     "$API" || fail "$API does not document the action call"
grep -q "PAGE_MENU"                       "$API" || fail "$API does not say the pages arrive as PAGE_MENU"
pass "docs/API.md documents both calls, the guard, and the page kind"

echo "OK: chain-UI trailing pages"
