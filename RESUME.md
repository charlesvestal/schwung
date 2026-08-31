# RESUME — Movy embeds Schwung's UI

Parked 2026-08-31. Working end to end on hardware behind a build flag.

Read `docs/plans/2026-08-31-movy-embeds-schwung-ui-status.md` first — it has the
split of duties, the six known gaps, and every device-found defect with its
cause. This file is just how to get back to a running build.

---

## The two checkouts

| | |
| --- | --- |
| **schwung** | `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/.claude/worktrees/param-pages-embeddable`<br>branch `worktree-param-pages-embeddable`, 7 commits ahead of `main` |
| **movy** | `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-movy-embed`<br>branch `schwung-grid-delta`, 13 ahead of `DimaDake/schwung-movy` `main` |

**Neither is pushed. The movy branch has no fork on GitHub.** It was originally
in a session scratchpad under `/private/tmp` and was moved here to survive; a
single directory is still a single point of failure, so push it to a fork if
this is going anywhere.

`node_modules` and `dist` were not copied into the movy checkout — see below.

---

## Get it building

```bash
export SCHWUNG=/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/.claude/worktrees/param-pages-embeddable

cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-movy-embed
npm install
npm run build:browser
```

`SCHWUNG` is not optional. movy imports Schwung's `param_pages` by its absolute
DEVICE path (`/data/UserData/schwung/shared/param_pages/...`), which the device
build leaves external and the browser build resolves via that variable. Without
it the import falls back to a stub that THROWS when called — deliberately, so a
stubbed grid cannot render blank and look like success.

---

## The checks

All in the movy checkout, all need `SCHWUNG` set. Each fails on the mutation of
the bug it was written for — do not trust one that has never failed.

```bash
node scripts/schwung-app-check.mjs           # the REAL app loop renders Schwung
node scripts/schwung-page-kinds-check.mjs    # preset/items/knobs all draw
node scripts/schwung-interaction-check.mjs   # knobs + clicks reach the controller
node scripts/schwung-knob-feel-check.mjs     # knob travel matches movy's
node scripts/schwung-late-contract-check.mjs # late module, empty slot, failed read
node scripts/schwung-pagination-check.mjs    # lanes follow parameters, not slots
node scripts/schwung-grid-delta.mjs          # measurement: how different the grids are
```

movy's own suite, which must stay green with the flag OFF:

```bash
node browser-test/screenshot.mjs   # 138 baselines
node browser-test/app-loop.mjs
node browser-test/logic.mjs
node browser-test/dump-replay.mjs
```

(`abi-parity.mjs` and `track-colors.mjs` skip: they want a schwung checkout at
megadake's own hardcoded `/Users/dake/git/cld/schwung`. Pre-existing.)

Schwung's suite, from the worktree:

```bash
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
```
213 pass at park.

---

## The switch

```
MOVY_SCHWUNG_GRID=page    Schwung plans and draws the module pages
MOVY_SCHWUNG_GRID=off     stock movy, byte-identical (the default)
```

It is a build-time define, not a runtime setting, so an ordinary build cannot
ship the experiment by forgetting a call.

---

## Deploy

Schwung first — movy's device build imports the shared library from it, so a
stale host means missing exports.

```bash
cd $SCHWUNG
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

If `build.sh` fails on the Link SDK, the submodule is not initialised in this
worktree (worktrees do not inherit submodules):

```bash
git submodule update --init --recursive libs/link
```

Then movy — the WHOLE module, not just `ui.js`:

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-movy-embed
MOVY_SCHWUNG_GRID=page ./scripts/build-module.sh
scp dist/movy-module.tar.gz ableton@move.local:/data/UserData/m.tar.gz
ssh ableton@move.local "cd /data/UserData/schwung/modules/tools \
  && tar -xzf /data/UserData/m.tar.gz && rm -f /data/UserData/m.tar.gz"
```

**Ship `dsp.so` and `module.json` with `ui.js` whenever the movy source has
moved.** A `ui.js`-only deploy is only safe while the installed `dsp.so` came
from the same tree — that assumption failed once already, when the device was
running an engine 65 commits behind the UI, and any oddity would have been
unattributable.

`scripts/build-module.sh` needs the Rust cross-target:
`rustup target add aarch64-unknown-linux-gnu` (the Homebrew linker is already
installed).

---

## The device

Running branch builds of BOTH. Rollback:

- **movy** → `/data/UserData/movy-backup-20260828/` holds the v0.29.0 `dsp.so`,
  `module.json` and `ui.js.pre-schwung-grid`. Copy them back into
  `/data/UserData/schwung/modules/tools/movy/` (renaming the ui file).
- **schwung** → reinstall whatever build you want. The device had no
  `version.txt` before this work, so its previous state was never recorded.

Do not leave a backup inside `modules/tools/` — it carries a `module.json` and
the scanner lists it as a second Movy. That is what caused the duplicate Tools
entry; the backup lives outside the scanned tree now.

Diagnostics, if the grid is not taking the frame:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "grep -E 'schwung-body|schwung-view' /data/UserData/schwung/debug.log | tail"
```

`schwung-body` names which of its early returns it took (`mode=`, `no-model`,
`step-page-selected`, `not-ready track= ck= pages=`, or `ok`). `schwung-view`
reports the view, session mode and masterDetail — that line is what identified
the wrong-view bug in one shot.

---

## Where to pick up

Not more features. The next question is whether movy WANTS this: it deletes
their knob renderer's reason to exist, couples their look to Schwung's release
cadence, and re-paginates every module. The technical case is made and
measured; the maintenance case is megadake's to make.

If you do keep building, the unrouted gestures are the small stuff — Back
(Schwung's exit intent), the section picker, and opening a divable param's
editor. All three have their plumbing in place (`click()`, `knobTurn()`,
`knobTouch()` on the page object); nothing consumes the intents yet.
