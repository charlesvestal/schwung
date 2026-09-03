# Cross-core module attribution for the CPU page

**Date:** 2026-09-03
**Status:** Design approved, not implemented
**Branch:** `manager-cpu-view` (continues PR #393)
**Predecessor:** `docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md`

The CPU page attributes per-module cost from the shim's frame-budget snapshot.
That is correct for a module whose DSP runs in the SPI callback, and **badly
wrong for one that forks.**

---

## The problem, measured

JP-8000 (`jp8000`, the JE-8086 emulation) is fork-parallel. In
`src/dsp/jp8000_plugin.cpp`:

- `create_instance` — **which is the SPI callback** — calls `fork()` (line
  ~1570) and keeps `inst->child_pid`.
- That child forks again, once per pipeline stage (line ~1382), and
  `pin_to_core`s each one.

So the work happens in a two-level process tree on cores 0-2, while the page's
frame-budget row shows only the dispatch-and-collect step left in the callback.
On the device it read **~5%**, which is true and deeply misleading.

**None of those processes can be identified by name.** A fork inherits the
parent's `comm`, and JP-8000 never calls `prctl(PR_SET_NAME)`. Observed on the
device, MoveOriginal's children were:

```
927  display-server     core0
934  schwung-manager    core2
973  link-subscriber    core0
975  Audio Main/SPI     core3   <- a forked PROCESS wearing the SPI thread's name
981  shadow_ui          core0
```

That last one is the whole difficulty in one line.

### Second problem, same page

Slot rows showed `(name unread)`. The manager log gave the cause:

```
07:40:41.930  shared memory params: not available (not on device)
07:40:41.931  perf snapshot shm: connected
07:40:43.965  shared memory params: connected (lazy)
07:40:48.105  cpu page: no param channel, module names unavailable
```

`App.shmParams` was nil at startup — the manager beat the shim, the same boot
race already fixed for `/schwung-perf` — and **stayed nil for the life of the
process.** `RemoteUI.ensureShm()` independently re-attaches into `ru.shm`, a
*second* mapping of the same segment, so the manager runs two handles and every
consumer reading the `App` field degrades silently.

---

## Design

### 1. Module identity comes from disk

Reading identity over the param channel costs 12 SPI-served requests per
refresh and depends on a channel shared with the shadow UI. It is all on disk
already.

**The schema, exactly** — every line below is something this design got wrong
once before checking:

```
active_set.txt      line 1 = the set uuid       NOT the newest mtime
                                                (27 sets existed; mtime order
                                                 and glob order both pointed at
                                                 the wrong one)
slot_N.json         chain.synth.module          NOT synth.module
                    chain.audio_fx[].type       "type", not "module"
                    chain.midi_fx[].type
master_fx_N.json    module_id                   different key again
```

An empty position is `{}` or a null `synth`, and must read as empty rather than
as a failure.

**Freshness guard.** Disk lags a hot swap until autosave writes. So: if disk
says a position is empty **but the snapshot shows nonzero timing for it**, that
is a contradiction, and only then do we fall back to a single param read for
that position. Steady state costs zero reads; a swap self-corrects instead of
being mislabelled. The telemetry we already collect is what detects the
disagreement — no extra polling to find out.

### 2. Cross-core attribution

Walk MoveOriginal's descendants **recursively** — JP-8000's stage workers are
grandchildren, so a one-level scan misses the actual DSP.

Subtract the helpers the shim itself spawns: `display-server`,
`schwung-manager`, `link-subscriber`, `shadow_ui`. Everything remaining is
module-forked and gets a row: pid, core, CPU% of one core.

Naming, in priority order:

1. **Declared** — a loaded module's `module.json` sets
   `capabilities.forks_processes: true`. Authoritative. Metadata only, no code
   change in the module. JP-8000 gets it.
2. **Inferred** — nothing declares, and exactly one synth is loaded. Attribute
   to it **and mark the row as inferred on screen.** An unlabelled guess on a
   CPU page is worse than no answer.
3. **Unattributed** — several candidates, or none loaded. Group them, show the
   real CPU, name no owner.

The current device is precisely the ambiguous case (`jp8000` *and* `9w9` both
loaded), which is why the declared flag carries the weight and inference is
only a fallback for modules nobody has flagged yet.

**Nothing is ever hidden for want of a name.** A forked process always appears
with its real cost, whatever we can or cannot call it. This rule has already
been broken twice on this page and is the one that matters most.

A declared forker's frame-budget row gains a pointer to its children, so the 5%
and the real cost are visibly connected rather than sitting in two tables that
look unrelated.

### 3. One param handle, lazily attached

`App.params()` — a lazy accessor mirroring the existing `App.perfSegment()`.
Every consumer uses it. `RemoteUI` shares that handle instead of keeping its
own, removing the duplicate mapping.

This fixes the class of bug rather than the instance: the next consumer to read
`App.shmParams` directly would have hit the same nil.

### 4. What the page must not claim

Carried forward from the predecessor spec, because every one of these has been
violated at least once here:

- A failed read reports failed. Never zeros, never an empty table.
- An inferred attribution is visibly marked as inferred.
- A fork-parallel module's frame-budget row states that it understates.
- A process we cannot name still shows its cost.

---

## Testing

- **Tree walk** over synthetic `/proc` fixtures: two-level fork, helpers
  present, the observed JP-8000 shape, and a child that exits mid-scan.
- **Attribution decision table**: declared / inferred-single / ambiguous /
  none-loaded, asserting the *inferred* case is flagged.
- **Disk parsing**: the real schema above, the `{}` empty case, a null `synth`,
  a missing `active_set.txt`, and an `active_set.txt` naming a set directory
  that does not exist.
- **Freshness fallback**: disk empty + nonzero timing triggers exactly one
  param read; disk populated triggers none.
- **Render**: an inferred attribution is labelled; an unattributed group still
  shows its CPU.

Each mutated once to prove it can fail, per the standing rule that a probe
which cannot fail reports green for the wrong reason.

---

## Out of scope

- Attributing forked children *per slot*. The tree gives us the module, not
  which slot instantiated it, and JP-8000's own `flock` allows only one
  pipeline per device anyway.
- Changing how any module forks. This measures what exists.
- cgroup or namespace isolation per module.
- History / time-series. Still a later, separable change.
