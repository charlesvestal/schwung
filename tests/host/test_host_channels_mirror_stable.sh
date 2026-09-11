#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# host.channels.stable and the top-level host fields describe the SAME
# release to two different audiences, and nothing in the format makes
# them agree.
#
# A Schwung Manager that predates channel support has no Channels field
# on CatalogHost, so encoding/json drops the block and it upgrades from
# host.latest_version / host.download_url. A manager that has it
# resolves host.channels.stable first and only falls back to the top
# level when that is absent.
#
# So the moment the two disagree the fleet splits in half by manager
# version, silently, with both halves believing they are on stable.
# That is not a visible failure — each device offers *a* coherent
# upgrade — which is exactly why it needs a test rather than care.
#
# Only stable is constrained. beta is free to name anything: it is the
# channel nothing falls back to, and it is meant to run ahead.

node - <<'NODE'
const fs = require("fs");

const CATALOG = "module-catalog.json";
const host = JSON.parse(fs.readFileSync(CATALOG, "utf8")).host;

let failures = 0;
const fail = (msg) => { console.error("FAIL: " + msg); failures++; };

if (!host) {
  fail(`${CATALOG} has no host block`);
} else if (!host.channels || !host.channels.stable) {
  // Absent is valid — an older-shaped catalog resolves entirely through
  // the top level. Nothing to keep in sync.
  console.log("host.channels.stable absent — nothing to mirror, OK");
} else {
  const s = host.channels.stable;
  if (s.version !== host.latest_version) {
    fail(`host.channels.stable.version (${s.version}) != host.latest_version (${host.latest_version}).\n` +
         `      Managers with channel support would install ${s.version} while older ones install ${host.latest_version}.`);
  }
  if (s.download_url !== host.download_url) {
    fail(`host.channels.stable.download_url != host.download_url.\n` +
         `      stable: ${s.download_url}\n` +
         `      top:    ${host.download_url}`);
  }
  if (!s.version || !s.download_url) {
    fail("host.channels.stable must carry both version and download_url; " +
         "a half-filled entry resolves to an empty download and the upgrade dies with no URL");
  }
  if (failures === 0) {
    console.log(`host.channels.stable mirrors the top level (${s.version}), OK`);
  }
}

// A beta, if present, must at least be complete — the resolver reads
// both fields and a missing download_url is an upgrade with nowhere to go.
if (host && host.channels && host.channels.beta) {
  const b = host.channels.beta;
  if (!b.version || !b.download_url) {
    fail("host.channels.beta must carry both version and download_url");
  } else {
    console.log(`host.channels.beta present (${b.version}), complete, OK`);
  }
}

process.exit(failures === 0 ? 0 : 1);
NODE
