#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THREE files name the current host release, and nothing in the repo made
# them agree.
#
#   src/host/version.txt              what the device reports it is running
#   module-catalog.json host.*        what the manager OFFERS and downloads
#   release.json                      the repo-level manifest
#
# They agreed at every release tag up to v0.12.1 and then stopped: from
# v1.0.0 onward release.json was left at 0.12.1 while the other two moved
# through fourteen releases. Nothing caught it because nothing reads
# release.json today -- the manager takes both the offer and the download
# URL from the catalog's host block, install.sh uses the releases API, and
# the catalog site builds its own snapshot from `gh api releases/latest`.
# The one reader is the retired on-device store's store_utils.mjs, which
# OVERWRITES catalog.host.latest_version with it; that path compares with
# isNewerVersion, so a 1.4.0 device saw "up to date" rather than an offered
# downgrade, and the drift stayed invisible.
#
# It drifted because release.yml deliberately does not commit it (main is
# branch-protected; a bot push is rejected GH006 and would fail the whole
# workflow after a successful publish), delegating instead to "the release
# PR" -- and the release checklist did not mention the file. A checklist
# line is what already failed here, so this is the enforcement.
#
# Sibling: test_host_channels_mirror_stable.sh pins agreement WITHIN the
# catalog (host.channels.stable vs the top-level fields). This one pins
# agreement ACROSS the three files.

node - <<'NODE'
const fs = require("fs");

let failures = 0;
const fail = (msg) => { console.error("FAIL: " + msg); failures++; };

const readJSON = (path) => {
  try {
    return JSON.parse(fs.readFileSync(path, "utf8"));
  } catch (e) {
    fail(`${path} is missing or not valid JSON: ${e.message}`);
    return null;
  }
};

const versionTxt = (() => {
  try {
    return fs.readFileSync("src/host/version.txt", "utf8").trim();
  } catch (e) {
    fail(`src/host/version.txt is unreadable: ${e.message}`);
    return null;
  }
})();

const catalog = readJSON("module-catalog.json");
const release = readJSON("release.json");
const host = catalog && catalog.host;
if (catalog && !host) fail("module-catalog.json has no host block");

if (versionTxt !== null && host) {
  if (host.latest_version !== versionTxt) {
    fail(`module-catalog.json host.latest_version (${host.latest_version}) != ` +
         `src/host/version.txt (${versionTxt}).\n` +
         `      The manager offers what the catalog names, so these disagreeing ` +
         `means every device is\n` +
         `      offered an "update" to, or told it is behind, a version that is not ` +
         `the one that shipped.`);
  }
}

if (versionTxt !== null && release) {
  if (release.version !== versionTxt) {
    fail(`release.json version (${release.version}) != src/host/version.txt ` +
         `(${versionTxt}).\n` +
         `      Bump release.json in the release PR -- release.yml cannot commit it ` +
         `(branch protection),\n` +
         `      which is exactly why it sat at 0.12.1 through fourteen releases.`);
  }
}

// A bumped version beside a stale URL downloads the OLD tarball while
// reporting the new number -- the same shape as the stale-catalog
// downgrade, from the other direction.
const urlNamesVersion = (url, version, where) => {
  if (!url) {
    fail(`${where} has no download_url; an upgrade resolves to an empty URL and dies`);
    return;
  }
  if (!url.includes(`/v${version}/`)) {
    fail(`${where} download_url does not name v${version}:\n      ${url}\n` +
         `      A bumped version with a stale URL downloads the old tarball and ` +
         `reports the new one.`);
  }
};

if (host && host.latest_version) {
  urlNamesVersion(host.download_url, host.latest_version, "module-catalog.json host");
}
if (release && release.version) {
  urlNamesVersion(release.download_url, release.version, "release.json");
}

if (failures === 0) {
  console.log(`version.txt, module-catalog.json host and release.json all name ` +
              `${versionTxt}, with matching URLs, OK`);
}
process.exit(failures === 0 ? 0 : 1);
NODE
