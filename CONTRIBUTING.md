# Contributing to Schwung

Thanks for hacking on Schwung — it's a community project and PRs are welcome.

## Workflow

`main` is protected. **Direct pushes to `main` are blocked** (for everyone,
maintainers included), so every change lands through a pull request that passes
CI:

1. Branch off `main`, or fork: `git switch -c my-change`
2. Commit your work.
3. Push and open a PR against `main`.
4. Wait for the three required checks to go green.
5. A maintainer merges once it's green.

We use a **trunk-based** flow — short-lived feature branches, no long-lived
`develop` branch. Merging code to `main` is *not* the same as shipping: the host
only reaches devices when a maintainer bumps `host.latest_version` in
`module-catalog.json` and pushes a release tag. Releases are cut with tags
(`git tag vX.Y.Z && git push --tags`), which are unaffected by branch protection.
See the Release Checklist in [CLAUDE.md](CLAUDE.md) and [BUILDING.md](BUILDING.md).

> The catalog *is* served live from `main`, so edits to `module-catalog.json`
> (module entries, descriptions) take effect for users as soon as they merge.

## Continuous integration

Every PR and push to `main` runs
[`.github/workflows/ci.yml`](.github/workflows/ci.yml). Three required jobs:

| Job | What it proves |
|-----|----------------|
| `host-tests` | Device-free unit + contract/invariant tests: `make -C tests/host test` and all `tests/host/*.sh`. |
| `go` | `go vet` / `build` / `test` for `schwung-manager`. |
| `cross-compile` | The app actually compiles for ARM64 (the same Docker build `release.yml` uses), and every expected artifact is present and `aarch64`. |

CI is **fork-safe by design**: it uses the `pull_request` trigger (never
`pull_request_target`), a read-only token, and no repository secrets. A
first-time contributor's workflow run needs a maintainer to click
**"Approve and run"** before it executes — that's a GitHub security gate, not a
failure.

> Only `tests/host/` is wired into CI. The broader `tests/{shadow,store,build}`
> suites still contain stale failures and are run manually.

## Run the checks locally

Install the pre-commit hook once per clone. It runs the two fast jobs
(`host-tests` + `go`, ~10s, no Docker or ARM toolchain) on every commit:

```bash
./scripts/install-hooks.sh
```

Bypass a single commit with `git commit --no-verify`, or
`SCHWUNG_SKIP_HOOKS=1 git commit …`. The ARM64 `cross-compile` job is
intentionally CI-only (it needs Docker + the toolchain, ~2.5 min).

## On-device testing

CI proves the code compiles and the contracts hold — it does **not** run on Move
hardware. Behavior changes (audio, MIDI, UI, timing) still need manual
verification on a device before release. See the Testing section of
[CLAUDE.md](CLAUDE.md) for the unified logger and the opt-in on-device E2E
harness (`tools/pytest-schwung/`).

## Code style & module requirements

Conventions live in [CLAUDE.md](CLAUDE.md) (C `snake_case` with `mm_`/`js_`
prefixes, `.mjs` shared modules vs `.js` UI, lowercase-hyphenated module IDs,
`lowercase_underscored` param keys). New modules go through the module-development
guide in [docs/MODULES.md](docs/MODULES.md) and must ship a `help.json` plus
screen-reader support (`src/shared/screen_reader.mjs`).
