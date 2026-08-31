# Module Channels (Beta / Stable)

Schwung Manager decides which version of a module to offer by reading
`release.json` from the module's repo. The channel feature lets one
`release.json` carry two versions — one for the default `stable`
channel and one for an opt-in `beta` channel — so a module author can
ship a pre-release to adventurous users without pushing it to
everyone.

Users pick a channel with the Stable / Beta toggle at the top of the
Modules page. The default is **Stable** and nobody is opted into
Beta automatically.

## Contract

`release.json` is strictly additive. The single-module shape stays:

```json
{
  "version": "1.2.3",
  "download_url": "https://.../mymod-1.2.3.tar.gz"
}
```

To publish a beta alongside stable, add an optional `channels` block:

```json
{
  "version": "1.2.3",
  "download_url": "https://.../mymod-1.2.3.tar.gz",
  "channels": {
    "stable": {"version": "1.2.3",         "download_url": "https://.../mymod-1.2.3.tar.gz"},
    "beta":   {"version": "1.3.0-beta.1",  "download_url": "https://.../mymod-1.3.0-beta.1.tar.gz"}
  }
}
```

Older Schwung Managers that don't know about channels read the
top-level `version` and `download_url` and behave exactly as they do
today — they see stable, always. The channels block is a hint only.

Multi-module release.json (one repo publishing several catalog entries)
takes the same optional `channels` per entry:

```json
{
  "modules": {
    "mymod":     {"version": "1.2.3", "download_url": "...", "channels": {"beta": {...}}},
    "mymod-lite":{"version": "1.2.3", "download_url": "..."}
  }
}
```

## Resolution rules

For a user on **stable**:

1. If `channels.stable` is set, use it.
2. Otherwise use the top-level `version` and `download_url`.

For a user on **beta**:

1. If `channels.beta.version` is strictly newer than the stable
   version (per semver-ish comparison), use the beta entry.
2. Otherwise fall back to stable.

Rule 2 is what quietly lands a beta user back on stable once stable
catches up. Beta users are never stranded on an older beta because the
author forgot to bump the beta entry when cutting a stable release.

Version comparison uses Schwung Manager's `isNewerSemver`, which is a
tolerant dotted-number compare — `1.3.0-beta.2 > 1.3.0-beta.1 > 1.2.3`.

## Tag convention for authors

The recommended release workflow is:

- Tag `vX.Y.Z-<pre>.N` (e.g. `v0.9.0-beta.1`, `v1.0.0-rc.2`) → publish
  only into `channels.beta`. Leave `channels.stable` (and the top-level
  version/URL) pointing at whatever the last stable release was.

- Tag `vX.Y.Z` (no pre-release suffix, e.g. `v1.3.0`) → publish into
  BOTH `channels.stable` AND `channels.beta` (they'll be identical),
  and update the top-level `version` / `download_url` to match. Once
  the beta entry is at the same version as stable, the resolver falls
  back to stable, so beta users automatically pick up the new stable.

This keeps every existing release.json valid — a repo that never
publishes betas can leave things exactly as they are.

## Example: adopting this from a stock module workflow

Most modules ship with a GitHub Actions workflow that overwrites
`release.json` on each tag. The minimal change is a two-branch script
based on the tag suffix. Something like this shell snippet inside the
workflow:

```bash
#!/usr/bin/env bash
set -euo pipefail

TAG="${GITHUB_REF##*/}"           # v0.9.0-beta.1 or v0.9.0
VERSION="${TAG#v}"
URL="https://github.com/${GITHUB_REPOSITORY}/releases/download/${TAG}/mymod-module.tar.gz"

# Preserve whatever's already in release.json (so a beta cut doesn't
# forget the last stable release).
if [ -f release.json ]; then
  PREV=$(cat release.json)
else
  PREV='{}'
fi

if [[ "$VERSION" == *-* ]]; then
  # Pre-release tag: only touch channels.beta. Top-level and
  # channels.stable stay pointed at the last stable.
  jq --arg v "$VERSION" --arg u "$URL" \
    '.channels.beta = {version: $v, download_url: $u}' \
    <<<"$PREV" > release.json
else
  # Stable tag: point stable and top-level at this release; also
  # advance beta to match so beta users fall back onto stable.
  jq --arg v "$VERSION" --arg u "$URL" \
    '.version = $v
     | .download_url = $u
     | .channels.stable = {version: $v, download_url: $u}
     | .channels.beta   = {version: $v, download_url: $u}' \
    <<<"$PREV" > release.json
fi

git add release.json
git -c user.name="release-bot" -c user.email="release-bot@example.com" \
  commit -m "release ${TAG}"
git push origin HEAD:main
```

Wire that into whichever `push`/`release` workflow already publishes
tarballs; the tag suffix decides which channel entries get rewritten.

## What Schwung Manager shows

- The version listed next to each module is the version the current
  channel would install. Betas are tagged with a small **beta** pill.
- Users on Stable see a subtle "beta v… available" hint next to
  modules whose beta entry is ahead of stable — a quiet nudge to
  opt in, not a modal.
- The Update button becomes active whenever the channel-resolved
  version is newer than what's installed, so a beta user gets Update
  buttons for the module's newest beta build.

The chosen channel is stored in
`/data/UserData/schwung/manager-cache/manager-config.json` under the
key `"module_channel"`. Nothing outside Schwung Manager reads that
file — on-device UIs don't fetch module updates.

## Non-goals

- No signing, staged rollouts, or telemetry.
- No automatic enrollment — the user picks the channel.
- No per-module opt-in (yet). If experience shows users want beta for
  one module but not others, this can extend without changing the
  manifest contract; the resolver stays the same, only the "channel to
  ask for" broadens from one global to a per-ID map.
