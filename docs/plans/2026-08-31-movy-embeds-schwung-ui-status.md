# Movy embeds Schwung's UI — status at park

**Parked 2026-08-31.** Working end to end on hardware, behind a build flag, with
a list of known gaps rather than open questions.

Design and Phase 0/1 background: `2026-08-28-param-pages-embeddable.md`.

## What it is

Movy no longer draws parameter UI. Schwung's `page_controller` draws it, Movy
keeps its header and footer, and the split of duties is:

| | |
| --- | --- |
| **Schwung** | publishes the UI, plans the page set, draws every page kind |
| **Movy** | page selection (jog), interaction (knob/touch/click), and the port |
| **Movy** | its own header and footer, and all of its own screens |

Movy does the param I/O; Schwung does none (rule 1 of `param_pages`). The
sequencer targets **parameters**, never page/slot.

## Where the code is

- **schwung** — this worktree, branch `worktree-param-pages-embeddable`.
- **movy** — a clone of `DimaDake/schwung-movy`, branch `schwung-grid-delta`,
  13 commits ahead of its `main` (merged to v0.30.0). **Not pushed, no fork
  created.** It lives in a scratchpad, so it is not durable — clone and
  re-apply, or push it somewhere, before relying on it.

Movy's side is four files plus checks: `renderer/schwung-page.ts` (the binding),
`renderer/schwung-grid.ts` (registry + mode), `renderer/schwung-body.ts` (the
earlier body-only path, superseded), and edits to `app/tick.ts`,
`midi/router.ts`, `renderer/{knob,chain}-view.ts`.

Built with `MOVY_SCHWUNG_GRID=page`; default `off` is byte-identical movy.
Local builds resolve Schwung's absolute device path via `SCHWUNG=/path/to/schwung`.

## On the device right now

Branch builds of **both**. Rollback:

- movy → `/data/UserData/movy-backup-20260828/` (v0.29.0, pre-experiment)
- schwung → reinstall whatever build you want; there was no `version.txt` on the
  device before this work, so its previous state was not recorded.

## Known gaps

Not bugs so much as unrouted or undecided:

1. **Back is not routed.** Schwung's exit intent (`page_input` returns
   `{action:"exit"}`) is ignored, so Back does movy's thing. Left deliberately:
   it is the gesture most likely to fight movy's own navigation.
2. **The section picker is unreachable** — jog-click with nothing held. The
   plumbing (`click()`) exists.
3. **A divable param cannot be opened.** The controller returns an "open"
   intent and the host must present the editor; movy has no such screen and
   nothing consumes the intent.
4. **The chain view's bank bar counts chain slots**, so it does not move as you
   jog through param pages. Correct for that view, confusing next to a body
   that is paging. Undecided.
5. **The ~19% restyle** is a taste call — measured, not yet judged.
6. **Screen reader**: `announce` is a no-op in the binding; movy has its own path.

## What earned a test, and why

Every one of these was found on the device after passing everything local, so
each has a check that fails on the mutation:

| Symptom reported | Cause |
| --- | --- |
| "drawing movy widgets not schwung" | routed `VIEW_KNOBS`; movy opens on `VIEW_CHAIN`, which calls `drawKnobParams` itself |
| "I opened braids, I see movy UI" | the contract read at construction came back empty and **latched** — no retry |
| "the presets page doesn't render" | the binding re-implemented planning and called the knob renderer, which draws knob pages only |
| "if I choose None I don't get kicked out" | `ready` latched the other way — a departed module still claimed a page set |
| "knobs move very very slowly like shift is held" | encoder **magnitude** discarded; `onKnobTurn` moves one detent per call |
| (found while fixing p-locks) | turn asked Schwung, touch/release asked movy — one gesture, three parameters |

`scripts/schwung-{app,page-kinds,late-contract,interaction,knob-feel}-check.mjs`,
plus `schwung-{grid-delta,pagination}-check.mjs` for the measurements.

## The lesson worth keeping

**Nearly every defect here was a probe that chose conditions it could not fail
under**, and the same three shapes recur:

- **Proving the piece instead of the wiring.** Checks drove `schwung-page`
  directly and passed while the app loop called movy's renderer.
- **Comparing on a case where both answers agree.** The router check used a
  preset where both planners put `freq` on knob 0, so deleting the routing
  still wrote `freq`.
- **A latched verdict.** Three separate times: an empty read latching
  "not ready", a good one latching "ready", and metadata settling from a failed
  read. `docs/SHADOW_UI.md`'s tri-state rule covers all three and I re-derived
  it each time.

And one measurement lied for a fourth reason: **the harness ran in zero wall
clock** while Schwung throttles writes by time, so a working write path looked
broken and I hunted it for a while.

## If this is picked up again

The next honest step is not more features — it is deciding whether movy WANTS
this. It is a large change to someone else's repo: it deletes their knob
renderer's reason to exist, couples their look to Schwung's release cadence,
and re-paginates every module. The technical case is made and measured; the
maintenance case is megadake's to make.
