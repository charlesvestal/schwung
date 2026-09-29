#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# `standalone` is ONE decision, made in src/shared/tool_launch.mjs:
#   - both spellings (top-level, capabilities.standalone) are honoured -- the
#     shadow UI used to read only the top-level one while module_manager.c's
#     text matcher found either, so the host that runs on the device ignored
#     the capability spelling;
#   - it WINS over every tool_config branch -- it used to be tested last, so
#     beside tool_config.interactive it was silently ignored.
# The shadow UI must route through the helper rather than restating the order.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { declaresStandalone, toolLaunchKind } from "./src/shared/tool_launch.mjs";
let failures = 0;
const eq = (what, got, want) => { if (got !== want) { console.error("FAIL: " + what + ": got " + got + ", want " + want); failures++; } };

eq("top-level", declaresStandalone({ standalone: true }), true);
eq("capability", declaresStandalone({ capabilities: { standalone: true } }), true);
eq("absent", declaresStandalone({ capabilities: {} }), false);
eq("false", declaresStandalone({ standalone: false }), false);
eq("string is not true", declaresStandalone({ standalone: "true" }), false);
eq("null json", declaresStandalone(null), false);

eq("standalone beats interactive", toolLaunchKind({ standalone: true, tool_config: { skip_file_browser: true, interactive: true } }), "standalone");
eq("standalone beats set_picker", toolLaunchKind({ standalone: true, tool_config: { set_picker: true } }), "standalone");
eq("standalone beats file browser", toolLaunchKind({ standalone: true, tool_config: { command: "x" } }), "standalone");
eq("set_picker", toolLaunchKind({ tool_config: { set_picker: true, interactive: true } }), "set_picker");
eq("interactive direct", toolLaunchKind({ tool_config: { skip_file_browser: true, interactive: true } }), "interactive");
eq("file browser", toolLaunchKind({ tool_config: { interactive: true } }), "file_browser");
eq("engines", toolLaunchKind({ tool_config: { engines: [] } }), "file_browser");
eq("nothing", toolLaunchKind({ tool_config: {} }), "unavailable");
process.exit(failures ? 1 : 0);
'

fail=0
grep -q 'toolLaunchKind(tool)' src/shadow/shadow_ui.js || { echo "FAIL: launchToolConfirmed does not route through toolLaunchKind" >&2; fail=1; }
grep -q 'declaresStandalone(json)' src/shadow/shadow_ui_tools.mjs || { echo "FAIL: scanForToolModules does not use declaresStandalone" >&2; fail=1; }
if grep -q 'json\.standalone' src/shadow/shadow_ui_tools.mjs; then echo "FAIL: shadow_ui_tools.mjs reads json.standalone directly again" >&2; fail=1; fi
body=$(awk '/^function launchToolConfirmed\(/{f=1} f{print} f&&/^}/{exit}' src/shadow/shadow_ui.js)
if printf '%s\n' "$body" | grep -q 'tool\.tool_config\.\(set_picker\|interactive\|skip_file_browser\)'; then
    echo "FAIL: launchToolConfirmed restates the dispatch order instead of asking toolLaunchKind" >&2; fail=1
fi
[ $fail -eq 0 ] && echo "PASS: tool launch standalone"
exit $fail
