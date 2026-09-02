# Manager CPU View Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `/system/cpu` page to schwung-manager showing where the device's CPU goes, broken down by module (from shim frame-budget instrumentation) and by system process (from `/proc`).

**Architecture:** The shim's existing per-slot timing — already unconditional, already paid — gains averages and two new call sites (Master FX, overtake DSP), and its snapshot struct moves out of `src/schwung_shim.c` into a shared header published as a seqlock-protected `/schwung-perf` SHM segment. The manager mmaps that segment, joins it with a `/proc` process table, and renders both under clearly separate headings. Collection is always-on; the 1 Hz polling is armed by a button on the page.

**Tech Stack:** C11 (shim + shared header), Go 1.x + `html/template` + htmx (manager), `tests/host/` (C units + shell pins), `go test`.

**User decisions (already made):**
- "Full: shim SHM + /proc" — per-module attribution is in scope, not just a process table.
- "only activated on the page with a 'measure CPU' button" — polling is armed, not automatic.
- "is monitoring performance impacting?" → answered in the spec; collection stays always-on, polling is what the button arms.
- "don't deploy anything else" — **nothing in this plan deploys.** No `install.sh`, no `scp`, no service restart. Task 0 is a read-only SSH query and is user-gated.
- "manager and shim ship together" — the deploy coupling is accepted; the plan documents it and the page reports a version mismatch explicitly.

**Spec:** `docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md`

**Branch:** `manager-cpu-view` (worktree `.claude/worktrees/manager-cpu-view`)

---

## File Structure

| File | Responsibility |
|---|---|
| `src/host/perf_snapshot.h` | **New.** The snapshot struct, SHM name/magic/version, size asserts. The single definition both the shim and the offsets test read. |
| `src/schwung_shim.c` | **Modify.** Delete the local `spi_timing_snapshot_t`; include the header; make `spi_snap` a pointer; add slot averages, Master FX and overtake timing, seqlock bracketing. |
| `src/host/shim_worker.c` | **Modify.** Attach `/schwung-perf` on the worker (never the SPI path), retrying until it succeeds. |
| `tests/host/test_perf_snapshot_size.c` | **New.** Size assert holds, shrinking fails, `magic`/`version` are first. |
| `tests/host/test_perf_shm_offsets.sh` | **New.** Compiles a C probe that prints `offsetof` for every field and diffs it against the Go const block. |
| `schwung-manager/perf_shm.go` | **New.** mmap + seqlock read of `/dev/shm/schwung-perf`. Nothing else. |
| `schwung-manager/perf_proc.go` | **New.** `/proc` parsing only: `/proc/stat`, `/proc/<pid>/stat`, `/proc/loadavg`, thread scan. Pure functions over strings + thin readers. |
| `schwung-manager/perf.go` | **New.** Joins the two, resolves slot → module id, owns the delta state and the two HTTP handlers. |
| `schwung-manager/perf_shm_test.go`, `perf_proc_test.go`, `perf_test.go` | **New.** Unit tests for the above. |
| `schwung-manager/templates/system_cpu.html` | **New.** Idle page + [Measure CPU]. |
| `schwung-manager/templates/partials/cpu_values.html` | **New.** The polled partial (carries `hx-trigger`). |
| `schwung-manager/templates/partials/cpu_idle.html` | **New.** What [Stop] swaps back in. |
| `schwung-manager/static/style.css` | **Modify.** A handful of rules for the ranked bars. |
| `schwung-manager/templates/base.html` | **Modify.** Nothing — `/system/cpu` keeps `Active: "system"`. Listed so nobody adds a nav item. |
| `docs/DIAGNOSTICS.md`, `CLAUDE.md` | **Modify.** One section + one index bullet. |

Three Go files rather than one: `perf_shm.go` and `perf_proc.go` are pure data acquisition with no knowledge of each other, and both are independently testable. `perf.go` is the only file that knows the page exists.

---

### Task 0: Confirm `clock_gettime` is vDSO-backed on the device

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Establish whether the shim's ~78 `clock_gettime` calls per SPI frame cost ~2–3 µs (vDSO) or ~150 µs (syscall), because the spec's "under 0.1%" claim rests on it.

**This does not deploy anything.** It is two read-only `cat` commands over SSH. **Ask before running it** — per the standing rule, don't measure the device while it is in use.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md` (record the result)

**Acceptance Criteria:**
- [ ] `/sys/devices/system/clocksource/clocksource0/current_clocksource` has been read from the device and its value recorded verbatim in the spec.
- [ ] A `vdso` mapping is confirmed present in `/proc/self/maps` on the device.
- [ ] The spec's "Is monitoring performance-impacting?" section states the measured clocksource instead of "the vDSO assumption is the one number to measure".
- [ ] If the clocksource is **not** `arch_sys_counter`, the finding is written up as its own paragraph — it means the shim's *existing* instrumentation is far more expensive than documented, which is a bug in its own right and independent of this feature.

**Verify:** `ssh ableton@move.local "cat /sys/devices/system/clocksource/clocksource0/current_clocksource; grep -c vdso /proc/self/maps"` → expected `arch_sys_counter` and a count `>= 1`

**Steps:**

- [ ] **Step 1: Ask the user whether the device is free**

Do not run anything until they say yes. If they decline, mark this task blocked and proceed to Task 1 — nothing else in the plan depends on it.

- [ ] **Step 2: Read the clocksource and confirm the vDSO is mapped**

```bash
ssh ableton@move.local "cat /sys/devices/system/clocksource/clocksource0/current_clocksource; \
  grep -c vdso /proc/self/maps; \
  cat /sys/devices/system/clocksource/clocksource0/available_clocksource"
```

Expected: `arch_sys_counter`, then `1`, then a list containing `arch_sys_counter`.

On ARM64, glibc's `clock_gettime(CLOCK_MONOTONIC)` takes the vDSO fast path (a `CNTVCT_EL0` read, no syscall) **only** when the kernel's current clocksource is the architected timer. Any other clocksource makes the vDSO fall through to a real `syscall`, at roughly 40–60× the cost.

- [ ] **Step 3: Record the result in the spec**

Replace the blockquote in the spec's "1. Existing RT timing" subsection with the measured value. If the clocksource is `arch_sys_counter`, the claim stands as written. If it is anything else, replace the "under 0.1%" figure with the recomputed one and add a paragraph flagging it as a pre-existing defect.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md
git commit -m "docs: record the measured clocksource behind the vDSO claim"
```

```json:metadata
{"files": ["docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md"], "verifyCommand": "ssh ableton@move.local \"cat /sys/devices/system/clocksource/clocksource0/current_clocksource; grep -c vdso /proc/self/maps\"", "acceptanceCriteria": ["current_clocksource read from the device and recorded verbatim in the spec", "vdso mapping confirmed present in /proc/self/maps", "spec states the measured clocksource instead of an assumption", "a non-arch_sys_counter result is written up as its own finding"], "userGate": true, "tags": ["user-gate"], "gateScope": "device-readonly", "modelTier": "standard"}
```

---

### Task 1: The shared snapshot header and its size test

**Goal:** `src/host/perf_snapshot.h` exists, defines the SHM contract, and is pinned by a C unit test that fails on a shrunken container or a misplaced `magic`/`version`.

**Files:**
- Create: `src/host/perf_snapshot.h`
- Create: `tests/host/test_perf_snapshot_size.c`
- Modify: `tests/host/Makefile` (add the target)

**Acceptance Criteria:**
- [ ] `magic` and `version` are the first two fields, in that order, at offsets 0 and 4.
- [ ] `_Static_assert(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE)` compiles.
- [ ] A floor assert exists so the container can only grow, never shrink.
- [ ] The test fails when `SCHWUNG_PERF_SHM_SIZE` is reduced below `sizeof`.
- [ ] `make -C tests/host test` passes with the new target included.

**Verify:** `make -C tests/host test 2>&1 | grep perf_snapshot` → `PASS test_perf_snapshot_size`

**Steps:**

- [ ] **Step 1: Write the header**

Create `src/host/perf_snapshot.h`:

```c
/* perf_snapshot.h — the SPI frame-budget snapshot, published over SHM.
 *
 * This struct used to be a file-static `spi_timing_snapshot_t` inside
 * src/schwung_shim.c, readable only by the shim's own background logger. The
 * manager's CPU page needs it, so it lives here and is published to
 * /schwung-perf.
 *
 * WHY THIS EXISTS AT ALL: modules are not processes. Every slot synth, slot
 * FX, Master FX and overtake DSP is a .so running on the SPI callback inside
 * MoveOriginal, so /proc can report what MoveOriginal costs in total but can
 * never split it by module. These numbers are the only per-module attribution
 * that exists.
 *
 * WRITER: the SPI callback, single writer, no locks. The stores are the ones
 * the shim already performed into a static — publishing costs no memcpy.
 * READER: any process, via the seqlock below.
 */

#ifndef PERF_SNAPSHOT_H
#define PERF_SNAPSHOT_H

#include <assert.h>
#include <stdint.h>

#define SHM_SCHWUNG_PERF      "/schwung-perf"
#define SCHWUNG_PERF_MAGIC    0x50455246u   /* "PERF" */
#define SCHWUNG_PERF_VERSION  1u

/* A whole page, deliberately. /dev/shm is tmpfs and allocates by page: measured
 * on the device, an 84-byte segment occupied 4096 — the same 8 blocks as a
 * 512-byte one. So headroom is free, and the asserts below are written so that
 * adding a field costs nothing and only SHRINKING fails the build. See
 * CLAUDE.md, "An SHM buffer sized to `sizeof` reads as FULL, and is not". */
#define SCHWUNG_PERF_SHM_SIZE 4096

/* Chain slots and Master FX slots. Restated rather than included because this
 * header is read by tests/host/ on the dev machine, which does not build the
 * chain manager. test_perf_snapshot_size.c pins them against the real headers. */
#define PERF_CHAIN_SLOTS    4
#define PERF_MASTER_FX_SLOTS 8

typedef struct {
    /* magic and version MUST stay the first two fields.
     *
     * A segment left behind by an older shim may be SHORTER than this struct,
     * and touching the tail of an undersized mapping is SIGBUS. Keeping the
     * version check itself inside the first 8 bytes means it can always be read
     * safely off whatever is actually there. This is the LINK_AUDIO_IN_SHM
     * lesson; do not reorder. */
    uint32_t magic;
    uint32_t version;

    /* Seqlock. The writer does seq++ … stores … seq++, so an ODD value means a
     * write is in flight and a reader that sees the same EVEN value before and
     * after read a consistent snapshot. */
    uint32_t seq;

    uint32_t frame_ready;         /* 1 = frame-level fields valid */
    uint32_t granular_ready;      /* 1 = section + slot fields valid */

    /* How many SPI frames the averages below cover. A consumer that does not
     * know the window cannot tell a real average from a partial one. */
    uint32_t sample_window_frames;

    /* The denominator, MEASURED rather than assumed.
     *
     * This is frame_total_avg: the whole loop iteration, which is paced by the
     * blocking ioctl and therefore sits at the SPI frame period (~2710-2830 us
     * measured; 128 frames / 44100 Hz = 2902 us nominal). Using the measured
     * value means a percentage cannot silently drift from reality if the period
     * ever changes. NOTE this is why total_us is not a load signal on its own —
     * our work shrinks the driver's wait by the same amount. */
    uint64_t frame_period_us;

    /* Frame-level timing, avg/max over the last sample_window_frames. */
    uint64_t frame_total_avg, frame_total_max;
    uint64_t frame_pre_avg, frame_pre_max;
    uint64_t frame_ioctl_avg, frame_ioctl_max;
    uint64_t frame_post_avg, frame_post_max;

    /* Granular pre-ioctl sections. */
    uint64_t midi_mon_avg, midi_mon_max;
    uint64_t fwd_midi_avg, fwd_midi_max;
    uint64_t mix_audio_avg, mix_audio_max;
    uint64_t ui_req_avg, ui_req_max;
    uint64_t param_req_avg, param_req_max;
    uint64_t fwd_cc_avg, fwd_cc_max;
    /* proc_midi is where MIDI FX cost lands. MIDI FX have no per-frame render —
     * they run event-driven inside the chain host's on_midi — so they are NOT
     * separable per module and must never be presented as if they were. */
    uint64_t proc_midi_avg, proc_midi_max;
    uint64_t jack_stash_avg, jack_stash_max;
    uint64_t drain_dsp_avg, drain_dsp_max;
    uint64_t jack_wake_avg, jack_wake_max;
    uint64_t mix_buf_avg, mix_buf_max;
    uint64_t tts_avg, tts_max;
    uint64_t display_avg, display_max;
    uint64_t clear_leds_avg, clear_leds_max;
    uint64_t jack_midi_avg, jack_midi_max;
    uint64_t ui_midi_avg, ui_midi_max;
    uint64_t flush_leds_avg, flush_leds_max;
    uint64_t screenreader_avg, screenreader_max;
    uint64_t jack_pre_avg, jack_pre_max;
    uint64_t jack_disp_avg, jack_disp_max;
    uint64_t pin_avg, pin_max;

    /* Post-ioctl chunks. */
    uint64_t post_midi_scan_avg, post_midi_scan_max;
    uint64_t post_drain_dsp_avg, post_drain_dsp_max;
    uint64_t post_render_avg, post_render_max;

    /* Per-slot render breakdown. The _max entries already existed; the _avg
     * entries are new and are the point of this work — a max over a ~3 s window
     * is a SPIKE DETECTOR, not a load figure. */
    uint64_t slot_render_avg[PERF_CHAIN_SLOTS], slot_render_max[PERF_CHAIN_SLOTS];
    uint64_t slot_synth_avg[PERF_CHAIN_SLOTS],  slot_synth_max[PERF_CHAIN_SLOTS];
    uint64_t slot_fx_avg[PERF_CHAIN_SLOTS],     slot_fx_max[PERF_CHAIN_SLOTS];

    /* Per-Master-FX-slot, new call site. */
    uint64_t mfx_avg[PERF_MASTER_FX_SLOTS], mfx_max[PERF_MASTER_FX_SLOTS];

    /* Overtake DSP, new call sites. */
    uint64_t overtake_gen_avg, overtake_gen_max;
    uint64_t overtake_fx_avg,  overtake_fx_max;

    uint32_t slot_probe_burst_max;
    uint32_t jack_audio_hits;
    uint32_t jack_audio_misses;

    uint32_t overrun_count;
    uint64_t last_overrun_total, last_overrun_pre;
    uint64_t last_overrun_ioctl, last_overrun_post;
} schwung_perf_snapshot_t;

/* Only SHRINKING fails the build. Adding a field is free until it crosses the
 * page, at which point raise SCHWUNG_PERF_SHM_SIZE and bump the version. */
_Static_assert(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE,
               "schwung_perf_snapshot_t outgrew its segment - raise "
               "SCHWUNG_PERF_SHM_SIZE and bump SCHWUNG_PERF_VERSION");
_Static_assert(SCHWUNG_PERF_SHM_SIZE >= 4096,
               "SCHWUNG_PERF_SHM_SIZE must never shrink below one page - "
               "tmpfs allocates by page, so a smaller container saves nothing "
               "and only makes the next field addition a breaking change");
_Static_assert(__builtin_offsetof(schwung_perf_snapshot_t, magic) == 0,
               "magic must be the first field - the version check has to be "
               "readable off a short segment left by an older shim");
_Static_assert(__builtin_offsetof(schwung_perf_snapshot_t, version) == 4,
               "version must be the second field");

#endif /* PERF_SNAPSHOT_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_perf_snapshot_size.c`:

```c
/* Unit test: the /schwung-perf container can only grow.
 *
 * The failure this guards against is the CONTROL_BUFFER_SIZE one: a buffer
 * sized to `sizeof` with an equality assert reads as a hard limit, so the next
 * person to need a field squeezes it into spare bits of an existing one
 * instead. It costs nothing to have headroom (tmpfs allocates by page), so the
 * asserts are written to fail only on a SHRINK.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "perf_snapshot.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void)
{
    /* The version check must be readable off a segment shorter than the whole
     * struct — otherwise the check itself is the thing that SIGBUSes. */
    CHECK(offsetof(schwung_perf_snapshot_t, magic) == 0,
          "magic must be at offset 0");
    CHECK(offsetof(schwung_perf_snapshot_t, version) == 4,
          "version must be at offset 4");
    CHECK(offsetof(schwung_perf_snapshot_t, seq) == 8,
          "seq must follow version - the Go mirror assumes it");

    CHECK(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE,
          "struct must fit its segment");

    /* Headroom is the point. If this ever fails the container is being sized to
     * fit rather than to leave room, which is the mistake this test exists to
     * prevent. 256 bytes is roughly 32 more uint64 fields. */
    CHECK(SCHWUNG_PERF_SHM_SIZE - sizeof(schwung_perf_snapshot_t) >= 256,
          "the segment must keep at least 256 bytes of headroom so adding a "
          "field is free - size the container, not the struct");

    CHECK(SCHWUNG_PERF_SHM_SIZE >= 4096,
          "segment must be at least one page");

    /* Pin the slot counts against the real headers. If MASTER_FX_SLOTS is
     * raised and this header is not, the page silently reports half the
     * chain — the same failure test_master_fx_slot_routing guards. */
    CHECK(PERF_CHAIN_SLOTS == 4,
          "PERF_CHAIN_SLOTS must match SHADOW_CHAIN_INSTANCES");
    CHECK(PERF_MASTER_FX_SLOTS == 8,
          "PERF_MASTER_FX_SLOTS must match MASTER_FX_SLOTS in "
          "src/host/shadow_chain_mgmt.h");

    if (fails) {
        fprintf(stderr, "test_perf_snapshot_size: %d failure(s)\n", fails);
        return 1;
    }
    printf("PASS test_perf_snapshot_size\n");
    return 0;
}
```

- [ ] **Step 3: Add the Makefile target**

In `tests/host/Makefile`, append to the `TARGETS` list (after `$(BUILD_DIR)/test_shadow_metronome`, adding a `\` to the previous line):

```make
	$(BUILD_DIR)/test_shadow_metronome \
	$(BUILD_DIR)/test_perf_snapshot_size
```

Also pin the slot counts against the real headers so a raised cap fails the build here rather than silently halving the page. After the existing `MASTER_FX_SLOTS := $(shell ...)` block, add:

```make
# perf_snapshot.h restates the slot counts (tests/host/ does not build the chain
# manager). Fail the build if they drift rather than let the CPU page report
# half the Master FX chain.
PERF_MFX_SLOTS := $(shell awk '/^\#define PERF_MASTER_FX_SLOTS /{print $$3}' ../../src/host/perf_snapshot.h)
ifneq ($(strip $(PERF_MFX_SLOTS)),$(strip $(MASTER_FX_SLOTS)))
$(error PERF_MASTER_FX_SLOTS ($(PERF_MFX_SLOTS)) != MASTER_FX_SLOTS ($(MASTER_FX_SLOTS)) - update src/host/perf_snapshot.h)
endif
```

- [ ] **Step 4: Run the test — it must pass, then prove it can fail**

```bash
make -C tests/host test 2>&1 | grep perf_snapshot
```
Expected: `PASS test_perf_snapshot_size`

Now mutate to prove the probe measures something. A test that cannot fail reports green for the wrong reason:

```bash
sed -i.bak 's/#define SCHWUNG_PERF_SHM_SIZE 4096/#define SCHWUNG_PERF_SHM_SIZE 512/' src/host/perf_snapshot.h
rm -f build/tests/host/test_perf_snapshot_size
make -C tests/host test 2>&1 | grep -i "perf_snapshot\|error"
```
Expected: a **compile error** from the `_Static_assert` (the struct exceeds 512).

```bash
mv src/host/perf_snapshot.h.bak src/host/perf_snapshot.h
rm -f build/tests/host/test_perf_snapshot_size
make -C tests/host test 2>&1 | grep perf_snapshot
```
Expected: `PASS test_perf_snapshot_size`

> **Delete the binary before re-running.** ExtFS has 1-second mtime granularity, so an edit inside the same second leaves make convinced the target is current and you validate the previous build.

- [ ] **Step 5: Commit**

```bash
git add src/host/perf_snapshot.h tests/host/test_perf_snapshot_size.c tests/host/Makefile
git commit -m "perf: shared snapshot header for the /schwung-perf segment"
```

```json:metadata
{"files": ["src/host/perf_snapshot.h", "tests/host/test_perf_snapshot_size.c", "tests/host/Makefile"], "verifyCommand": "make -C tests/host test 2>&1 | grep perf_snapshot", "acceptanceCriteria": ["magic at offset 0 and version at offset 4", "static assert on sizeof <= SCHWUNG_PERF_SHM_SIZE compiles", "a floor assert prevents shrinking the container", "the test fails when SCHWUNG_PERF_SHM_SIZE is reduced below sizeof", "make -C tests/host test passes"], "modelTier": "standard"}
```

---

### Task 2: Shim publishes the snapshot

**Goal:** The shim writes its timing into `/schwung-perf` instead of a private static, with averages for the slots, new Master FX and overtake call sites, and seqlock bracketing.

**Files:**
- Modify: `src/schwung_shim.c` (delete the local struct ~5203-5257; slot timing ~1901-2090; MFX loop ~2733; overtake gen ~2097-2125; overtake FX ~2726; snapshot block ~8850-8900)
- Modify: `src/host/shim_worker.c` (attach on the worker)
- Modify: `src/host/shim_worker.h` (declare the attach hook)

**Acceptance Criteria:**
- [ ] `spi_timing_snapshot_t` no longer exists in `src/schwung_shim.c`; the shim includes `host/perf_snapshot.h`.
- [ ] `spi_snap` is a pointer; all existing `spi_snap.field` sites become `spi_snap->field` and still compile.
- [ ] With the segment unmapped, the shim writes into a static fallback and does not crash or branch on NULL per field.
- [ ] Per-slot synth/FX/render sums accumulate and produce `_avg` alongside the existing `_max`.
- [ ] The Master FX loop times each loaded slot into `mfx_avg[]` / `mfx_max[]`; empty slots add no `clock_gettime` calls.
- [ ] Overtake generator and FX are timed.
- [ ] `seq` is incremented before and after the snapshot stores, leaving it even at rest.
- [ ] `frame_period_us` is set from `frame_total_avg`, and `sample_window_frames` from the window count.
- [ ] `shm_open`/`mmap` happen only on the worker thread, never in a callback.
- [ ] `./scripts/build.sh` succeeds (cross-compile). **No deploy.**

**Verify:** `./scripts/build.sh 2>&1 | tail -5` → build succeeds, and `grep -c "spi_snap\." src/schwung_shim.c` → `0`

**Steps:**

- [ ] **Step 1: Replace the local struct with the shared header**

In `src/schwung_shim.c`, add near the other `host/` includes (by line 77, beside `#include "host/shadow_shm_util.h"`):

```c
#include "host/perf_snapshot.h"
```

Delete the whole `typedef struct { ... } spi_timing_snapshot_t;` block (currently ~5203-5255) and the line `static volatile spi_timing_snapshot_t spi_snap = {0};` (~5257). Replace with:

```c
/* The snapshot lives in /schwung-perf when the segment is mapped, and in this
 * static when it is not.
 *
 * A pointer rather than a copy-then-publish: the stores below are the ones the
 * shim already performed, so publishing costs no memcpy and adds nothing to the
 * SPI callback. The fallback means no store site needs a NULL check — there is
 * always somewhere to write. The worker swings the pointer once, after the
 * pages are faulted in; see shim_worker.c.
 *
 * `volatile` on the pointee, not the pointer: the reader is another process. */
static volatile schwung_perf_snapshot_t spi_snap_fallback;
static volatile schwung_perf_snapshot_t *spi_snap = &spi_snap_fallback;

/* Called from the worker thread ONLY (shm_open/mmap are not RT-safe). Idempotent. */
void shim_perf_publish_to(volatile schwung_perf_snapshot_t *dst)
{
    if (!dst || dst == spi_snap) return;
    /* Seed the new home with what we have so the first reader does not see a
     * zeroed snapshot and report "everything idle" — which is exactly the lie
     * a failed read must never tell. */
    memcpy((void *)dst, (const void *)spi_snap, sizeof(*dst));
    dst->magic = SCHWUNG_PERF_MAGIC;
    dst->version = SCHWUNG_PERF_VERSION;
    __sync_synchronize();
    spi_snap = dst;
}
```

Now rewrite every `spi_snap.` to `spi_snap->`:

```bash
sed -i.bak 's/spi_snap\./spi_snap->/g' src/schwung_shim.c && rm -f src/schwung_shim.c.bak
```

- [ ] **Step 2: Accumulate per-slot sums**

Beside the existing `spi_slot_render_max` declarations (search `static uint64_t spi_slot_render_max`), add the sums:

```c
static uint64_t spi_slot_render_sum[SHADOW_CHAIN_INSTANCES];
static uint64_t spi_slot_synth_sum[SHADOW_CHAIN_INSTANCES];
static uint64_t spi_slot_fx_sum[SHADOW_CHAIN_INSTANCES];
```

In the synth timing block (currently `if (synth_us > spi_slot_synth_max[s]) spi_slot_synth_max[s] = synth_us;`), add the sum on the line above:

```c
                spi_slot_synth_sum[s] += synth_us;
                if (synth_us > spi_slot_synth_max[s]) spi_slot_synth_max[s] = synth_us;
```

Same for FX (`if (fx_us > spi_slot_fx_max[s])`):

```c
                    spi_slot_fx_sum[s] += fx_us;
                    if (fx_us > spi_slot_fx_max[s]) spi_slot_fx_max[s] = fx_us;
```

And for the whole-slot timer at the end of the loop (`if (slot_us > spi_slot_render_max[s])`):

```c
            spi_slot_render_sum[s] += slot_us;
            if (slot_us > spi_slot_render_max[s]) spi_slot_render_max[s] = slot_us;
```

- [ ] **Step 3: Time the Master FX loop**

Add the accumulators next to the slot ones:

```c
static uint64_t spi_mfx_sum[MASTER_FX_SLOTS];
static uint64_t spi_mfx_max[MASTER_FX_SLOTS];
```

Replace the Master FX loop body (currently at `src/schwung_shim.c:2733`) with:

```c
    for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
        master_fx_slot_t *s = &shadow_master_fx_slots[fx];
        if (!(s->instance && s->api && s->api->process_block)) continue;
        int16_t mfx_dry[FRAMES_PER_BLOCK * 2];
        if (s->bypassed) {
            memcpy(mfx_dry, fx_target, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        }
        /* Timed inside the `continue` guard, so an empty slot costs nothing —
         * a chain with one effect loaded pays two clock reads, not sixteen. */
        struct timespec mfx_t0, mfx_t1;
        clock_gettime(CLOCK_MONOTONIC, &mfx_t0);
        s->api->process_block(s->instance, fx_target, FRAMES_PER_BLOCK);
        clock_gettime(CLOCK_MONOTONIC, &mfx_t1);
        uint64_t mfx_us = (mfx_t1.tv_sec - mfx_t0.tv_sec) * 1000000ULL +
                          (mfx_t1.tv_nsec - mfx_t0.tv_nsec) / 1000;
        spi_mfx_sum[fx] += mfx_us;
        if (mfx_us > spi_mfx_max[fx]) spi_mfx_max[fx] = mfx_us;
        if (s->bypassed) {
            memcpy(fx_target, mfx_dry, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        }
    }
```

- [ ] **Step 4: Time the overtake DSP**

Add:

```c
static uint64_t spi_overtake_gen_sum, spi_overtake_gen_max;
static uint64_t spi_overtake_fx_sum,  spi_overtake_fx_max;
```

At the overtake generator render (`src/schwung_shim.c:2125`), bracket the call:

```c
        struct timespec og_t0, og_t1;
        clock_gettime(CLOCK_MONOTONIC, &og_t0);
        overtake_dsp_gen->render_block(overtake_dsp_gen_inst, render_buffer, MOVE_FRAMES_PER_BLOCK);
        clock_gettime(CLOCK_MONOTONIC, &og_t1);
        {
            uint64_t og_us = (og_t1.tv_sec - og_t0.tv_sec) * 1000000ULL +
                             (og_t1.tv_nsec - og_t0.tv_nsec) / 1000;
            spi_overtake_gen_sum += og_us;
            if (og_us > spi_overtake_gen_max) spi_overtake_gen_max = og_us;
        }
```

At the overtake FX call (`src/schwung_shim.c:2727`, inside `if (!overtake_fx_eoc && ...)`), bracket the same way:

```c
        struct timespec of_t0, of_t1;
        clock_gettime(CLOCK_MONOTONIC, &of_t0);
        overtake_dsp_fx->process_block(overtake_dsp_fx_inst, fx_target, FRAMES_PER_BLOCK);
        clock_gettime(CLOCK_MONOTONIC, &of_t1);
        {
            uint64_t of_us = (of_t1.tv_sec - of_t0.tv_sec) * 1000000ULL +
                             (of_t1.tv_nsec - of_t0.tv_nsec) / 1000;
            spi_overtake_fx_sum += of_us;
            if (of_us > spi_overtake_fx_max) spi_overtake_fx_max = of_us;
        }
```

- [ ] **Step 5: Publish averages and bracket the snapshot with the seqlock**

In the granular snapshot block (`if (spi_granular_count >= 1000) {`), wrap the whole body. Immediately after the opening brace and `int n = spi_granular_count;`, add:

```c
        /* Seqlock: odd means a write is in flight. A reader that sees the same
         * EVEN value before and after its read got a consistent snapshot. Two
         * stores per ~1000 frames — this is not a cost. */
        spi_snap->seq++;
        __sync_synchronize();
```

Replace the existing per-slot publish loop with one that carries the averages, and add the new fields:

```c
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES && s < PERF_CHAIN_SLOTS; s++) {
            spi_snap->slot_render_max[s] = spi_slot_render_max[s];
            spi_snap->slot_synth_max[s]  = spi_slot_synth_max[s];
            spi_snap->slot_fx_max[s]     = spi_slot_fx_max[s];
            spi_snap->slot_render_avg[s] = spi_slot_render_sum[s] / n;
            spi_snap->slot_synth_avg[s]  = spi_slot_synth_sum[s] / n;
            spi_snap->slot_fx_avg[s]     = spi_slot_fx_sum[s] / n;
        }
        for (int fx = 0; fx < MASTER_FX_SLOTS && fx < PERF_MASTER_FX_SLOTS; fx++) {
            spi_snap->mfx_avg[fx] = spi_mfx_sum[fx] / n;
            spi_snap->mfx_max[fx] = spi_mfx_max[fx];
        }
        spi_snap->overtake_gen_avg = spi_overtake_gen_sum / n;
        spi_snap->overtake_gen_max = spi_overtake_gen_max;
        spi_snap->overtake_fx_avg  = spi_overtake_fx_sum / n;
        spi_snap->overtake_fx_max  = spi_overtake_fx_max;

        spi_snap->sample_window_frames = (uint32_t)n;
        /* The denominator, measured. frame_total_avg is the whole loop
         * iteration, which the blocking ioctl paces to the frame period. */
        spi_snap->frame_period_us = spi_snap->frame_total_avg;
```

Reset the new accumulators alongside the existing ones (in the block that already zeroes `spi_slot_render_max[s]` etc.):

```c
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            spi_slot_render_max[s] = 0;
            spi_slot_synth_max[s] = 0;
            spi_slot_fx_max[s] = 0;
            spi_slot_render_sum[s] = 0;
            spi_slot_synth_sum[s] = 0;
            spi_slot_fx_sum[s] = 0;
        }
        for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
            spi_mfx_sum[fx] = 0;
            spi_mfx_max[fx] = 0;
        }
        spi_overtake_gen_sum = spi_overtake_gen_max = 0;
        spi_overtake_fx_sum  = spi_overtake_fx_max  = 0;
```

Finally, replace the existing `spi_snap->seq++;` (which was a plain counter) at the end of the block — it is now the closing half of the seqlock, so it must come **after** every store, immediately before `spi_granular_count = 0;`:

```c
        __sync_synchronize();
        spi_snap->seq++;   /* back to EVEN — snapshot is consistent */
        spi_granular_count = 0;
```

> The old `spi_snap.seq++` sat in the middle of the block. Leaving it there makes the seqlock report consistency while stores are still landing, which is worse than no seqlock — the reader would trust a torn snapshot. Grep for a stray `seq++` before committing.

- [ ] **Step 6: Attach the segment on the worker**

In `src/host/shim_worker.h`, add near the other declarations:

```c
/* Attach /schwung-perf and hand the shim its publish target. Worker-only:
 * shm_open and mmap are not realtime-safe. No-op once attached. */
void perf_shm_attach_tick(void);
```

In `src/host/shim_worker.c`, add the include beside the others:

```c
#include "perf_snapshot.h"
#include "shadow_shm_util.h"
```

and the implementation, next to `spi_tally_tick`:

```c
/* ---- /schwung-perf publish -------------------------------------------- */

/* The CPU page's frame-budget panel reads this. Retried until it succeeds
 * rather than attempted once: /dev/shm may not be writable at the instant the
 * shim initialises, and a single silent failure would leave the page reporting
 * "shim not running" forever with nothing to say why.
 *
 * ALWAYS ON — no arming flag. The timing it publishes is already collected
 * unconditionally, so this costs one mmap and two stores per ~1000 frames.
 * Arming it would make the page blank by default, which reads as a broken
 * build (see docs/DIAGNOSTICS.md on the SPI tally's 20 s silence). */
extern void shim_perf_publish_to(volatile schwung_perf_snapshot_t *dst);

void perf_shm_attach_tick(void)
{
    static schwung_perf_snapshot_t *shm = NULL;
    static int moaned = 0;
    if (shm) return;

    shm = (schwung_perf_snapshot_t *)shadow_shm_map(
        SHM_SCHWUNG_PERF, SCHWUNG_PERF_SHM_SIZE, 1, 1);
    if (!shm) {
        if (!moaned) {
            moaned = 1;
            unified_log("shim", LOG_LEVEL_WARN,
                        "perf: could not create " SHM_SCHWUNG_PERF
                        " - the manager's CPU page will report no shim");
        }
        return;
    }

    /* Fault every page in before the SPI callback ever writes here. A first
     * touch from the callback is a page fault on the realtime thread. */
    memset(shm, 0, SCHWUNG_PERF_SHM_SIZE);

    shim_perf_publish_to((volatile schwung_perf_snapshot_t *)shm);
    unified_log("shim", LOG_LEVEL_INFO,
                "perf: publishing to " SHM_SCHWUNG_PERF " (v%u, %zu bytes)",
                SCHWUNG_PERF_VERSION, sizeof(schwung_perf_snapshot_t));
}
```

Call it from `worker_main`, beside the other ~1 Hz ticks:

```c
        if (tick % 5 == 0) poll_flags();          /* ~1 Hz */
        if (tick % 5 == 0) perf_shm_attach_tick();/* ~1 Hz until attached */
        if (tick % 5 == 0) rt_audit_tick();       /* ~1 Hz, no-op unless armed */
```

- [ ] **Step 7: Build and confirm no stale `spi_snap.` remains**

```bash
grep -c "spi_snap\." src/schwung_shim.c
```
Expected: `0`

```bash
grep -n "seq++" src/schwung_shim.c
```
Expected: exactly two lines, one immediately after `int n = spi_granular_count;` and one immediately before `spi_granular_count = 0;`.

```bash
./scripts/build.sh 2>&1 | tail -5
```
Expected: build succeeds. **Do not deploy.**

- [ ] **Step 8: Commit**

```bash
git add src/schwung_shim.c src/host/shim_worker.c src/host/shim_worker.h
git commit -m "perf: publish the SPI frame-budget snapshot to /schwung-perf

Adds per-slot averages (the existing entries were max-only, i.e. a spike
detector rather than a load figure), times the Master FX loop and the
overtake DSP, and brackets the snapshot with a seqlock so a reader in
another process cannot see a torn frame."
```

```json:metadata
{"files": ["src/schwung_shim.c", "src/host/shim_worker.c", "src/host/shim_worker.h"], "verifyCommand": "./scripts/build.sh 2>&1 | tail -5 && grep -c 'spi_snap\\.' src/schwung_shim.c", "acceptanceCriteria": ["spi_timing_snapshot_t removed from schwung_shim.c and the shared header included", "spi_snap is a pointer with a static fallback and all sites compile", "per-slot synth/FX/render sums produce _avg alongside _max", "Master FX loop times each loaded slot inside the continue guard", "overtake generator and FX are timed", "seq increments before and after all stores, leaving it even at rest", "frame_period_us set from frame_total_avg and sample_window_frames from the window", "shm_open/mmap happen only on the worker thread", "scripts/build.sh succeeds with no deploy"], "modelTier": "standard"}
```

---

### Task 3: Go seqlock reader for `/schwung-perf`

**Goal:** `perf_shm.go` maps the segment and returns a consistent snapshot, or reports the read as **failed** — never as zeros.

**Files:**
- Create: `schwung-manager/perf_shm.go`
- Create: `schwung-manager/perf_shm_test.go`

**Acceptance Criteria:**
- [ ] `OpenPerfShm()` returns `nil` when the segment is absent (not on device), matching `OpenShmParams`.
- [ ] `Read()` returns `(snapshot, nil)` only when `seq` was even and unchanged across the read.
- [ ] An odd `seq` retried 3 times returns `ErrPerfTorn`, not a zeroed snapshot.
- [ ] A wrong `magic` returns `ErrPerfMagic`; a wrong `version` returns a `PerfVersionError` naming both versions.
- [ ] Tests cover: good read, torn read, odd seq, bad magic, version mismatch.

**Verify:** `cd schwung-manager && go test ./... -run TestPerf -v` → all PASS

**Steps:**

- [ ] **Step 1: Write the failing tests**

Create `schwung-manager/perf_shm_test.go`:

```go
package main

import (
	"encoding/binary"
	"errors"
	"testing"
)

// buildSnapshot lays out a synthetic /schwung-perf payload. Offsets mirror
// src/host/perf_snapshot.h; test_perf_shm_offsets.sh pins them for real.
func buildSnapshot(magic, version, seq uint32) []byte {
	b := make([]byte, perfShmSize)
	binary.LittleEndian.PutUint32(b[perfOffMagic:], magic)
	binary.LittleEndian.PutUint32(b[perfOffVersion:], version)
	binary.LittleEndian.PutUint32(b[perfOffSeq:], seq)
	binary.LittleEndian.PutUint64(b[perfOffFramePeriodUs:], 2902)
	binary.LittleEndian.PutUint32(b[perfOffSampleWindow:], 1000)
	binary.LittleEndian.PutUint64(b[perfOffSlotSynthAvg:], 412)
	return b
}

func TestPerfReadGood(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 8)}
	snap, err := p.Read()
	if err != nil {
		t.Fatalf("wanted a clean read, got %v", err)
	}
	if snap.FramePeriodUs != 2902 {
		t.Fatalf("FramePeriodUs = %d, want 2902", snap.FramePeriodUs)
	}
	if snap.SlotSynthAvg[0] != 412 {
		t.Fatalf("SlotSynthAvg[0] = %d, want 412", snap.SlotSynthAvg[0])
	}
	if snap.SampleWindowFrames != 1000 {
		t.Fatalf("SampleWindowFrames = %d, want 1000", snap.SampleWindowFrames)
	}
}

// An odd seq means the writer is mid-snapshot. Returning zeros here would say
// "everything is idle", which is the exact lie a failed read must never tell.
func TestPerfOddSeqIsAFailureNotZeros(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 7)}
	snap, err := p.Read()
	if !errors.Is(err, ErrPerfTorn) {
		t.Fatalf("odd seq must report ErrPerfTorn, got err=%v", err)
	}
	if snap != nil {
		t.Fatal("a failed read must return no snapshot at all - a caller " +
			"handed zeros will draw a picture of an idle device")
	}
}

// seq changing between the two samples means the writer landed a snapshot
// while we were copying it.
func TestPerfTornReadIsReported(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion, 8)}
	p.testHookAfterCopy = func() {
		binary.LittleEndian.PutUint32(p.data[perfOffSeq:], 10)
	}
	_, err := p.Read()
	if !errors.Is(err, ErrPerfTorn) {
		t.Fatalf("a seq that moved during the read must report ErrPerfTorn, got %v", err)
	}
}

func TestPerfBadMagic(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(0xDEADBEEF, perfVersion, 8)}
	if _, err := p.Read(); !errors.Is(err, ErrPerfMagic) {
		t.Fatalf("wanted ErrPerfMagic, got %v", err)
	}
}

// A version mismatch is the deploy-coupling failure: shim and manager ship
// together, so the page must say so by name rather than render garbage.
func TestPerfVersionMismatchNamesBothVersions(t *testing.T) {
	p := &PerfShm{data: buildSnapshot(perfMagic, perfVersion+1, 8)}
	_, err := p.Read()
	var ve *PerfVersionError
	if !errors.As(err, &ve) {
		t.Fatalf("wanted a PerfVersionError, got %v", err)
	}
	if ve.Got != perfVersion+1 || ve.Want != perfVersion {
		t.Fatalf("PerfVersionError must carry both versions, got %+v", ve)
	}
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd schwung-manager && go test ./... -run TestPerf 2>&1 | head -20
```
Expected: FAIL — `undefined: PerfShm`, `undefined: perfOffMagic`, etc.

- [ ] **Step 3: Write the implementation**

Create `schwung-manager/perf_shm.go`:

```go
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sync"
	"syscall"
)

// PerfShm reads the SPI frame-budget snapshot the shim publishes.
//
// WHY THIS EXISTS: modules are not processes. Every slot synth, slot FX,
// Master FX and overtake DSP is a .so running on MoveOriginal's SPI callback,
// so /proc can say what MoveOriginal costs in total but can never split it by
// module. This segment is the only per-module attribution that exists.
//
// Offsets mirror schwung_perf_snapshot_t in src/host/perf_snapshot.h. They are
// hand-maintained, exactly like shmparams.go's — and exactly like that file,
// drift is silent and total. tests/host/test_perf_shm_offsets.sh compiles a C
// probe that prints offsetof for every field and diffs it against the const
// block below. If you add or reorder a field, that test is what catches it.
type PerfShm struct {
	data []byte
	mu   sync.Mutex

	// Test seam: lets a unit test simulate the writer landing a snapshot
	// between our copy and our second seq read. Nil in production.
	testHookAfterCopy func()
}

const perfShmPath = "/dev/shm/schwung-perf"

// Mirror of src/host/perf_snapshot.h.
const (
	perfMagic   = 0x50455246 // "PERF"
	perfVersion = 1
	perfShmSize = 4096

	perfChainSlots    = 4
	perfMasterFXSlots = 8
)

// Byte offsets into schwung_perf_snapshot_t.
//
// The first three are load-bearing beyond the usual: magic and version sit at
// 0 and 4 so the version check can be read off a SHORT segment left by an
// older shim. Touching the tail of an undersized mapping is SIGBUS, so the
// check that decides whether to touch the tail must itself be safe.
const (
	perfOffMagic        = 0
	perfOffVersion      = 4
	perfOffSeq          = 8
	perfOffFrameReady   = 12
	perfOffGranularRdy  = 16
	perfOffSampleWindow = 20
	perfOffFramePeriodUs = 24

	perfOffFrameTotalAvg = 32
	perfOffFrameTotalMax = 40
	perfOffFramePreAvg   = 48
	perfOffFramePreMax   = 56
	perfOffFrameIoctlAvg = 64
	perfOffFrameIoctlMax = 72
	perfOffFramePostAvg  = 80
	perfOffFramePostMax  = 88

	// The timed sections start here as ONE contiguous run of (avg, max) uint64
	// pairs: 21 granular pre-ioctl sections followed by 3 post-ioctl chunks of
	// exactly the same shape. They are walked as a single run of 24 because
	// that is what the C struct lays out — treating the post chunks as a
	// separate block and adding their size again is an easy 48-byte error, and
	// silent: every per-slot number would come from the wrong field.
	// Verified against the compiler: midi_mon_avg is at 96 and slot_render_avg
	// at 480, and 96 + 24*16 = 480.
	perfOffSections   = 96
	perfSectionCount  = 24
	perfSectionStride = 16

	// Where the 3 post-ioctl chunks begin within that run. Declared only so
	// the offsets test can check it; nothing indexes off it.
	perfGranularSectionCount = 21
	perfOffPostChunks        = perfOffSections + perfGranularSectionCount*perfSectionStride

	// Per-slot arrays, each PERF_CHAIN_SLOTS uint64s.
	perfOffSlotRenderAvg = perfOffSections + perfSectionCount*perfSectionStride
	perfOffSlotRenderMax = perfOffSlotRenderAvg + perfChainSlots*8
	perfOffSlotSynthAvg  = perfOffSlotRenderMax + perfChainSlots*8
	perfOffSlotSynthMax  = perfOffSlotSynthAvg + perfChainSlots*8
	perfOffSlotFxAvg     = perfOffSlotSynthMax + perfChainSlots*8
	perfOffSlotFxMax     = perfOffSlotFxAvg + perfChainSlots*8

	perfOffMfxAvg = perfOffSlotFxMax + perfChainSlots*8
	perfOffMfxMax = perfOffMfxAvg + perfMasterFXSlots*8

	perfOffOvertakeGenAvg = perfOffMfxMax + perfMasterFXSlots*8
	perfOffOvertakeGenMax = perfOffOvertakeGenAvg + 8
	perfOffOvertakeFxAvg  = perfOffOvertakeGenMax + 8
	perfOffOvertakeFxMax  = perfOffOvertakeFxAvg + 8

	perfOffProbeBurstMax = perfOffOvertakeFxMax + 8
	perfOffJackHits      = perfOffProbeBurstMax + 4
	perfOffJackMisses    = perfOffJackHits + 4
	perfOffOverrunCount  = perfOffJackMisses + 4
)

// perfSectionNames names the 24 granular sections in struct order. The order is
// the contract — a name inserted here without a matching field in the C struct
// silently relabels every section after it.
var perfSectionNames = []string{
	"MIDI monitor", "Forward MIDI", "Mix audio", "UI requests",
	"Param requests", "Forward CC",
	// MIDI FX have no per-frame render: they run event-driven inside the chain
	// host's on_midi, so their cost lands here and is NOT separable per module.
	// The page must label it, not attribute it.
	"Process MIDI (incl. MIDI FX)",
	"JACK stash", "Drain DSP", "JACK wake", "Mix buffer", "TTS",
	"Display", "Clear LEDs", "JACK MIDI out", "UI MIDI out", "Flush LEDs",
	"Screen reader", "JACK pre", "JACK display", "CPU pin",
	"Post MIDI scan", "Post drain DSP", "Post render",
}

var (
	// ErrPerfAbsent means the segment is not there — the shim is not running,
	// or this is not the device.
	ErrPerfAbsent = errors.New("perf: /schwung-perf not present")
	// ErrPerfMagic means the segment exists but holds something else.
	ErrPerfMagic = errors.New("perf: bad magic in /schwung-perf")
	// ErrPerfTorn means the seqlock never settled: the writer was mid-snapshot
	// every time we looked. NOT the same as "no load" — a caller that renders
	// zeros here draws a picture of an idle device from a read that failed.
	ErrPerfTorn = errors.New("perf: snapshot never settled (seqlock torn)")
)

// PerfVersionError is the deploy-coupling failure. Shim and manager ship
// together; when they have not, say so by name rather than render garbage.
type PerfVersionError struct{ Got, Want uint32 }

func (e *PerfVersionError) Error() string {
	return fmt.Sprintf("perf: /schwung-perf is version %d, this manager expects %d "+
		"— deploy the shim and the manager together", e.Got, e.Want)
}

// PerfSnapshot is one consistent read of the segment.
type PerfSnapshot struct {
	SampleWindowFrames uint32
	FramePeriodUs      uint64
	FrameReady         bool
	GranularReady      bool

	FrameTotalAvg, FrameTotalMax uint64
	FramePreAvg, FramePreMax     uint64
	FrameIoctlAvg, FrameIoctlMax uint64
	FramePostAvg, FramePostMax   uint64

	// Sections is parallel to perfSectionNames.
	SectionAvg [perfSectionCount]uint64
	SectionMax [perfSectionCount]uint64

	SlotRenderAvg, SlotRenderMax [perfChainSlots]uint64
	SlotSynthAvg, SlotSynthMax   [perfChainSlots]uint64
	SlotFxAvg, SlotFxMax         [perfChainSlots]uint64

	MfxAvg, MfxMax [perfMasterFXSlots]uint64

	OvertakeGenAvg, OvertakeGenMax uint64
	OvertakeFxAvg, OvertakeFxMax   uint64

	ProbeBurstMax uint32
	OverrunCount  uint32
}

// OpenPerfShm maps the segment. Returns nil when it is absent, matching
// OpenShmParams — the caller treats nil as "not on device".
func OpenPerfShm() *PerfShm {
	f, err := os.OpenFile(perfShmPath, os.O_RDONLY, 0)
	if err != nil {
		return nil
	}
	defer f.Close()

	// Never map more than the segment holds. A stale short segment left by an
	// older shim would otherwise SIGBUS on the first touch past its end — in
	// this process, at some arbitrary later moment. Same rule as
	// shadow_shm_map's fstat check on the C side.
	st, err := f.Stat()
	if err != nil || st.Size() < perfShmSize {
		return nil
	}

	data, err := syscall.Mmap(int(f.Fd()), 0, perfShmSize,
		syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil
	}
	return &PerfShm{data: data}
}

const perfReadAttempts = 3

// Read returns a consistent snapshot, or an error. It NEVER returns a
// zero-valued snapshot alongside a nil error: "the read failed" and "the
// device is idle" are different sentences and must not share a representation.
func (p *PerfShm) Read() (*PerfSnapshot, error) {
	if p == nil || len(p.data) < perfShmSize {
		return nil, ErrPerfAbsent
	}
	p.mu.Lock()
	defer p.mu.Unlock()

	if got := p.u32(perfOffMagic); got != perfMagic {
		return nil, fmt.Errorf("%w (got %#x)", ErrPerfMagic, got)
	}
	if got := p.u32(perfOffVersion); got != perfVersion {
		return nil, &PerfVersionError{Got: got, Want: perfVersion}
	}

	for attempt := 0; attempt < perfReadAttempts; attempt++ {
		before := p.u32(perfOffSeq)
		if before%2 != 0 {
			continue // writer is mid-snapshot
		}
		snap := p.decode()
		if p.testHookAfterCopy != nil {
			p.testHookAfterCopy()
		}
		if p.u32(perfOffSeq) == before {
			return snap, nil
		}
	}
	return nil, ErrPerfTorn
}

func (p *PerfShm) u32(off int) uint32 {
	return binary.LittleEndian.Uint32(p.data[off:])
}

func (p *PerfShm) u64(off int) uint64 {
	return binary.LittleEndian.Uint64(p.data[off:])
}

func (p *PerfShm) decode() *PerfSnapshot {
	s := &PerfSnapshot{
		SampleWindowFrames: p.u32(perfOffSampleWindow),
		FramePeriodUs:      p.u64(perfOffFramePeriodUs),
		FrameReady:         p.u32(perfOffFrameReady) == 1,
		GranularReady:      p.u32(perfOffGranularRdy) == 1,

		FrameTotalAvg: p.u64(perfOffFrameTotalAvg),
		FrameTotalMax: p.u64(perfOffFrameTotalMax),
		FramePreAvg:   p.u64(perfOffFramePreAvg),
		FramePreMax:   p.u64(perfOffFramePreMax),
		FrameIoctlAvg: p.u64(perfOffFrameIoctlAvg),
		FrameIoctlMax: p.u64(perfOffFrameIoctlMax),
		FramePostAvg:  p.u64(perfOffFramePostAvg),
		FramePostMax:  p.u64(perfOffFramePostMax),

		OvertakeGenAvg: p.u64(perfOffOvertakeGenAvg),
		OvertakeGenMax: p.u64(perfOffOvertakeGenMax),
		OvertakeFxAvg:  p.u64(perfOffOvertakeFxAvg),
		OvertakeFxMax:  p.u64(perfOffOvertakeFxMax),

		ProbeBurstMax: p.u32(perfOffProbeBurstMax),
		OverrunCount:  p.u32(perfOffOverrunCount),
	}
	for i := 0; i < perfSectionCount; i++ {
		off := perfOffSections + i*perfSectionStride
		s.SectionAvg[i] = p.u64(off)
		s.SectionMax[i] = p.u64(off + 8)
	}
	for i := 0; i < perfChainSlots; i++ {
		s.SlotRenderAvg[i] = p.u64(perfOffSlotRenderAvg + i*8)
		s.SlotRenderMax[i] = p.u64(perfOffSlotRenderMax + i*8)
		s.SlotSynthAvg[i] = p.u64(perfOffSlotSynthAvg + i*8)
		s.SlotSynthMax[i] = p.u64(perfOffSlotSynthMax + i*8)
		s.SlotFxAvg[i] = p.u64(perfOffSlotFxAvg + i*8)
		s.SlotFxMax[i] = p.u64(perfOffSlotFxMax + i*8)
	}
	for i := 0; i < perfMasterFXSlots; i++ {
		s.MfxAvg[i] = p.u64(perfOffMfxAvg + i*8)
		s.MfxMax[i] = p.u64(perfOffMfxMax + i*8)
	}
	return s
}
```

- [ ] **Step 4: Run the tests**

```bash
cd schwung-manager && go test ./... -run TestPerf -v 2>&1 | tail -20
```
Expected: all `PASS`.

- [ ] **Step 5: Prove the torn-read test can fail**

```bash
cd schwung-manager
sed -i.bak 's/return nil, ErrPerfTorn/return \&PerfSnapshot{}, nil/' perf_shm.go
go test ./... -run TestPerf 2>&1 | grep -c FAIL
mv perf_shm.go.bak perf_shm.go
go test ./... -run TestPerf 2>&1 | tail -3
```
Expected: a non-zero FAIL count under the mutation, then all PASS after restoring. A probe that cannot fail reports green for the wrong reason.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/perf_shm.go schwung-manager/perf_shm_test.go
git commit -m "manager: seqlock reader for /schwung-perf

A failed read reports failed. It never returns zeros with a nil error -
'the read did not complete' and 'the device is idle' are different
sentences and must not share a representation."
```

```json:metadata
{"files": ["schwung-manager/perf_shm.go", "schwung-manager/perf_shm_test.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestPerf -v", "acceptanceCriteria": ["OpenPerfShm returns nil when the segment is absent", "Read returns a snapshot only when seq was even and unchanged", "an odd seq retried 3 times returns ErrPerfTorn and a nil snapshot", "bad magic returns ErrPerfMagic", "version mismatch returns a PerfVersionError naming both versions", "the torn-read test fails under mutation"], "modelTier": "standard"}
```

---

### Task 4: Pin the Go offset mirror against the C header

**Goal:** A shell test compiles a C probe that prints `offsetof` for every field and diffs it against the Go const block, so drift fails CI instead of silently returning wrong numbers.

**Files:**
- Create: `tests/host/test_perf_shm_offsets.sh`

**Acceptance Criteria:**
- [ ] The test compiles a probe against `src/host/perf_snapshot.h` and extracts the Go constants from `schwung-manager/perf_shm.go`.
- [ ] It compares every offset the Go file declares.
- [ ] Inserting a field into the C struct makes it fail.
- [ ] It is executable and runs green from the repo root.

**Verify:** `bash tests/host/test_perf_shm_offsets.sh` → `PASS test_perf_shm_offsets`

**Steps:**

- [ ] **Step 1: Write the test**

Create `tests/host/test_perf_shm_offsets.sh`:

```bash
#!/usr/bin/env bash
# The Go reader mirrors schwung_perf_snapshot_t by hand. This compiles a probe
# that asks the compiler where each field ACTUALLY is and diffs that against
# the constants in perf_shm.go.
#
# Why this test earns its place: shmparams.go carries the same kind of mirror,
# and when two uint64 trace fields were added to shadow_param_t the Go side was
# not updated. Every key landed 16 bytes late, the shim read an empty key, and
# EVERY GET returned empty — the remote UI showed "no module" and default
# params. Nothing crashed and no test failed. Drift in a hand-mirrored layout
# is silent and total; only a probe like this one catches it.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1

HEADER=src/host/perf_snapshot.h
GOFILE=schwung-manager/perf_shm.go
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

[ -f "$HEADER" ] || { fail "missing $HEADER"; exit 1; }
[ -f "$GOFILE" ] || { fail "missing $GOFILE"; exit 1; }

# Fields the Go side reads directly, paired with the Go constant that names the
# offset. Arrays are checked at element 0; the Go side derives the rest by
# stride, and the stride is uint64 by construction.
FIELDS="
magic:perfOffMagic
version:perfOffVersion
seq:perfOffSeq
frame_ready:perfOffFrameReady
granular_ready:perfOffGranularRdy
sample_window_frames:perfOffSampleWindow
frame_period_us:perfOffFramePeriodUs
frame_total_avg:perfOffFrameTotalAvg
frame_total_max:perfOffFrameTotalMax
frame_pre_avg:perfOffFramePreAvg
frame_pre_max:perfOffFramePreMax
frame_ioctl_avg:perfOffFrameIoctlAvg
frame_ioctl_max:perfOffFrameIoctlMax
frame_post_avg:perfOffFramePostAvg
frame_post_max:perfOffFramePostMax
midi_mon_avg:perfOffSections
post_midi_scan_avg:perfOffPostChunks
slot_render_avg:perfOffSlotRenderAvg
slot_render_max:perfOffSlotRenderMax
slot_synth_avg:perfOffSlotSynthAvg
slot_synth_max:perfOffSlotSynthMax
slot_fx_avg:perfOffSlotFxAvg
slot_fx_max:perfOffSlotFxMax
mfx_avg:perfOffMfxAvg
mfx_max:perfOffMfxMax
overtake_gen_avg:perfOffOvertakeGenAvg
overtake_gen_max:perfOffOvertakeGenMax
overtake_fx_avg:perfOffOvertakeFxAvg
overtake_fx_max:perfOffOvertakeFxMax
slot_probe_burst_max:perfOffProbeBurstMax
jack_audio_hits:perfOffJackHits
jack_audio_misses:perfOffJackMisses
overrun_count:perfOffOverrunCount
"

# Build the probe.
{
  echo '#include <stdio.h>'
  echo '#include <stddef.h>'
  echo '#include "perf_snapshot.h"'
  echo 'int main(void) {'
  for pair in $FIELDS; do
    c="${pair%%:*}"
    printf '  printf("%s %%zu\\n", offsetof(schwung_perf_snapshot_t, %s));\n' "$c" "$c"
  done
  echo '  printf("SIZEOF %zu\n", sizeof(schwung_perf_snapshot_t));'
  echo '  return 0;'
  echo '}'
} > "$TMP/probe.c"

if ! cc -I src/host -o "$TMP/probe" "$TMP/probe.c" 2>"$TMP/cc.err"; then
    echo "FAIL: probe did not compile"
    cat "$TMP/cc.err"
    exit 1
fi
"$TMP/probe" > "$TMP/offsets.txt"

# Resolve the Go constants. They are written as arithmetic expressions
# (perfOffSlotRenderMax = perfOffSlotRenderAvg + perfChainSlots*8), so ask Go
# itself rather than trying to parse them.
cat > "$TMP/dump_test.go" <<'GOEOF'
package main

import (
	"fmt"
	"testing"
)

func TestDumpPerfOffsets(t *testing.T) {
	for _, e := range []struct {
		name string
		off  int
	}{
		{"magic", perfOffMagic},
		{"version", perfOffVersion},
		{"seq", perfOffSeq},
		{"frame_ready", perfOffFrameReady},
		{"granular_ready", perfOffGranularRdy},
		{"sample_window_frames", perfOffSampleWindow},
		{"frame_period_us", perfOffFramePeriodUs},
		{"frame_total_avg", perfOffFrameTotalAvg},
		{"frame_total_max", perfOffFrameTotalMax},
		{"frame_pre_avg", perfOffFramePreAvg},
		{"frame_pre_max", perfOffFramePreMax},
		{"frame_ioctl_avg", perfOffFrameIoctlAvg},
		{"frame_ioctl_max", perfOffFrameIoctlMax},
		{"frame_post_avg", perfOffFramePostAvg},
		{"frame_post_max", perfOffFramePostMax},
		{"midi_mon_avg", perfOffSections},
		{"post_midi_scan_avg", perfOffPostChunks},
		{"slot_render_avg", perfOffSlotRenderAvg},
		{"slot_render_max", perfOffSlotRenderMax},
		{"slot_synth_avg", perfOffSlotSynthAvg},
		{"slot_synth_max", perfOffSlotSynthMax},
		{"slot_fx_avg", perfOffSlotFxAvg},
		{"slot_fx_max", perfOffSlotFxMax},
		{"mfx_avg", perfOffMfxAvg},
		{"mfx_max", perfOffMfxMax},
		{"overtake_gen_avg", perfOffOvertakeGenAvg},
		{"overtake_gen_max", perfOffOvertakeGenMax},
		{"overtake_fx_avg", perfOffOvertakeFxAvg},
		{"overtake_fx_max", perfOffOvertakeFxMax},
		{"slot_probe_burst_max", perfOffProbeBurstMax},
		{"jack_audio_hits", perfOffJackHits},
		{"jack_audio_misses", perfOffJackMisses},
		{"overrun_count", perfOffOverrunCount},
	} {
		fmt.Printf("GOOFF %s %d\n", e.name, e.off)
	}
}
GOEOF

cp "$TMP/dump_test.go" schwung-manager/zz_perf_offsets_dump_test.go
(cd schwung-manager && go test -run TestDumpPerfOffsets -v ./... 2>/dev/null) \
    | grep '^GOOFF ' > "$TMP/gooffsets.txt"
rm -f schwung-manager/zz_perf_offsets_dump_test.go

if [ ! -s "$TMP/gooffsets.txt" ]; then
    echo "FAIL: could not read offsets out of $GOFILE"
    exit 1
fi

while read -r _ name goff; do
    coff=$(awk -v n="$name" '$1 == n {print $2}' "$TMP/offsets.txt")
    if [ -z "$coff" ]; then
        fail "$name: no C offset (field renamed or removed from $HEADER?)"
        continue
    fi
    if [ "$coff" != "$goff" ]; then
        fail "$name: C says $coff, $GOFILE says $goff"
    fi
done < "$TMP/gooffsets.txt"

# The Go side maps a fixed perfShmSize; a struct that outgrew it would be read
# past the end of what the mapping covers.
sizeof=$(awk '$1 == "SIZEOF" {print $2}' "$TMP/offsets.txt")
if [ "$sizeof" -gt 4096 ]; then
    fail "struct is $sizeof bytes, past the 4096 the Go reader maps"
fi

if [ "$fails" -ne 0 ]; then
    echo "test_perf_shm_offsets: $fails failure(s)"
    exit 1
fi
echo "PASS test_perf_shm_offsets"
```

```bash
chmod +x tests/host/test_perf_shm_offsets.sh
```

- [ ] **Step 2: Run it**

```bash
bash tests/host/test_perf_shm_offsets.sh
```
Expected: `PASS test_perf_shm_offsets`

- [ ] **Step 3: Prove it catches drift**

Insert a field into the C struct and confirm the test goes red:

```bash
sed -i.bak 's/    uint32_t frame_ready;/    uint64_t drift_probe;\n    uint32_t frame_ready;/' src/host/perf_snapshot.h
bash tests/host/test_perf_shm_offsets.sh
```
Expected: `FAIL: frame_ready: C says ... , schwung-manager/perf_shm.go says 12` plus several more, exit 1.

```bash
mv src/host/perf_snapshot.h.bak src/host/perf_snapshot.h
bash tests/host/test_perf_shm_offsets.sh
```
Expected: `PASS test_perf_shm_offsets`

- [ ] **Step 4: Commit**

```bash
git add tests/host/test_perf_shm_offsets.sh
git commit -m "test: pin the Go /schwung-perf offset mirror against the C header

shmparams.go carries the same kind of hand-mirrored layout, and when two
uint64s were added to shadow_param_t the Go side was not updated: every
key landed 16 bytes late and every GET returned empty, with nothing
crashing and no test failing. This is the probe that catches that."
```

```json:metadata
{"files": ["tests/host/test_perf_shm_offsets.sh"], "verifyCommand": "bash tests/host/test_perf_shm_offsets.sh", "acceptanceCriteria": ["compiles a C probe against perf_snapshot.h and resolves the Go constants", "compares every offset the Go file declares", "inserting a field into the C struct makes it fail", "runs green from the repo root and is executable"], "modelTier": "standard"}
```

---

### Task 5: `/proc` parsers

**Goal:** `perf_proc.go` turns `/proc` text into process, per-core and thread CPU numbers, with the `comm`-delimiting rule that `rt_thread_audit.c` gets right.

**Files:**
- Create: `schwung-manager/perf_proc.go`
- Create: `schwung-manager/perf_proc_test.go`

**Acceptance Criteria:**
- [ ] `parseProcStatLine` delimits `comm` on the **last** `)`, so `Audio Main/SPI` and `worker (2)` parse correctly.
- [ ] It returns `ok == false` when the line stops before field 41, rather than reporting `SCHED_OTHER 0`.
- [ ] `parseCPUStat` returns per-core busy/total jiffies from `/proc/stat`.
- [ ] `cpuPercent` divides by **measured** elapsed time, not an assumed interval.
- [ ] Tests cover: comm with a space, comm with a paren, a truncated line, a `/proc/stat` sample, and delta math over a known elapsed.

**Verify:** `cd schwung-manager && go test ./... -run TestProc -v` → all PASS

**Steps:**

- [ ] **Step 1: Write the failing tests**

Create `schwung-manager/perf_proc_test.go`:

```go
package main

import (
	"testing"
	"time"
)

// makeStatLine builds a /proc/<pid>/stat line: pid, (comm), then fields 3..41
// with utime at 14, stime at 15, processor at 39, rt_priority at 40,
// policy at 41.
func makeStatLine(pid int, comm string, utime, stime uint64, cpu, rtprio, policy int) string {
	fields := make([]string, 39) // fields 3..41
	for i := range fields {
		fields[i] = "0"
	}
	set := func(n int, v string) { fields[n-3] = v }
	set(14, itoa(utime))
	set(15, itoa(stime))
	set(39, itoaInt(cpu))
	set(40, itoaInt(rtprio))
	set(41, itoaInt(policy))
	return itoaInt(pid) + " (" + comm + ") S " + join(fields[1:], " ")
}

// Move's threads are literally named "Audio Main/SPI". Tokenising comm on
// whitespace shifts every field after it and silently reports every thread as
// SCHED_OTHER 0 — which reads as a clean all-clear, the worst possible wrong
// answer for a tool whose whole job is finding FIFO-70 threads.
func TestParseProcStatCommWithSpace(t *testing.T) {
	line := makeStatLine(1234, "Audio Main/SPI", 500, 100, 3, 70, 1)
	p, ok := parseProcStatLine(line)
	if !ok {
		t.Fatal("a comm containing a space must still parse")
	}
	if p.Comm != "Audio Main/SPI" {
		t.Fatalf("Comm = %q, want %q", p.Comm, "Audio Main/SPI")
	}
	if p.Policy != schedFIFO || p.RTPrio != 70 {
		t.Fatalf("policy/prio = %d/%d, want %d/70 - comm was tokenised on "+
			"whitespace and every field after it shifted", p.Policy, p.RTPrio, schedFIFO)
	}
	if p.Utime != 500 || p.Stime != 100 {
		t.Fatalf("utime/stime = %d/%d, want 500/100", p.Utime, p.Stime)
	}
	if p.CPU != 3 {
		t.Fatalf("CPU = %d, want 3", p.CPU)
	}
}

// A comm can contain parentheses too. Delimit on the LAST ')'.
func TestParseProcStatCommWithParen(t *testing.T) {
	line := makeStatLine(99, "worker (2)", 10, 20, 1, 0, 0)
	p, ok := parseProcStatLine(line)
	if !ok {
		t.Fatal("a comm containing parens must still parse")
	}
	if p.Comm != "worker (2)" {
		t.Fatalf("Comm = %q, want %q", p.Comm, "worker (2)")
	}
	if p.Policy != schedOther {
		t.Fatalf("Policy = %d, want SCHED_OTHER", p.Policy)
	}
}

// A line that stops before field 41 says nothing about scheduling. Reporting it
// as SCHED_OTHER 0 would be a false all-clear.
func TestParseProcStatTruncatedIsNotOK(t *testing.T) {
	if _, ok := parseProcStatLine("42 (short) S 1 2 3"); ok {
		t.Fatal("a truncated stat line must report !ok, never SCHED_OTHER 0")
	}
}

func TestParseCPUStat(t *testing.T) {
	sample := `cpu  100 0 50 800 0 0 0 0 0 0
cpu0 25 0 10 200 0 0 0 0 0 0
cpu1 25 0 10 200 0 0 0 0 0 0
cpu2 25 0 10 200 0 0 0 0 0 0
cpu3 25 0 20 200 0 0 0 0 0 0
intr 12345`
	cores, ok := parseCPUStat(sample)
	if !ok {
		t.Fatal("wanted a parse")
	}
	if len(cores) != 5 {
		t.Fatalf("got %d entries, want 5 (aggregate + 4 cores)", len(cores))
	}
	// cpu3: 25+0+20+200 = 245 total, 45 busy.
	if cores[4].Total != 245 || cores[4].Busy != 45 {
		t.Fatalf("cpu3 = busy %d / total %d, want 45/245",
			cores[4].Busy, cores[4].Total)
	}
}

// The delta must divide by MEASURED elapsed. Dividing by an assumed 1s makes a
// slow poll, a second browser, or a paused tab report a scaled-wrong number.
func TestCPUPercentUsesMeasuredElapsed(t *testing.T) {
	// 100 Hz clock: 50 ticks = 500ms of CPU.
	got := cpuPercent(50, 100, 2*time.Second)
	if got < 24.9 || got > 25.1 {
		t.Fatalf("500ms of CPU over 2s = %.2f%%, want 25%%", got)
	}
	// Same ticks over half the wall time is twice the load.
	got = cpuPercent(50, 100, 1*time.Second)
	if got < 49.9 || got > 50.1 {
		t.Fatalf("500ms of CPU over 1s = %.2f%%, want 50%%", got)
	}
}

func TestCPUPercentZeroElapsedIsZeroNotInfinity(t *testing.T) {
	if got := cpuPercent(50, 100, 0); got != 0 {
		t.Fatalf("zero elapsed must give 0, got %v", got)
	}
}
```

Add the helpers the test uses at the bottom of the same file, and add `strconv` and `strings` to its imports:

```go
func itoa(v uint64) string { return strconv.Itoa(int(v)) }
func itoaInt(v int) string { return strconv.Itoa(v) }
func join(a []string, sep string) string { return strings.Join(a, sep) }
```

so the test file's import block is:

```go
import (
	"strconv"
	"strings"
	"testing"
	"time"
)
```

- [ ] **Step 2: Run to verify failure**

```bash
cd schwung-manager && go test ./... -run TestProc 2>&1 | head -10
```
Expected: FAIL — `undefined: parseProcStatLine`.

- [ ] **Step 3: Write the implementation**

Create `schwung-manager/perf_proc.go`:

```go
package main

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Linux scheduling policies, spelled out so nothing here needs Linux headers.
const (
	schedOther = 0
	schedFIFO  = 1
	schedRR    = 2
)

// Field numbers in proc(5), 1-based, counting the bracketed comm as field 2.
const (
	fieldUtime     = 14
	fieldStime     = 15
	fieldProcessor = 39
	fieldRTPrio    = 40
	fieldPolicy    = 41
)

// ProcStat is one process or thread from /proc/<pid>/stat.
type ProcStat struct {
	PID    int
	Comm   string
	Utime  uint64 // clock ticks
	Stime  uint64 // clock ticks
	CPU    int    // last core it ran on
	RTPrio int
	Policy int
}

// IsRealtime reports whether this is FIFO or RR at a nonzero priority.
func (p ProcStat) IsRealtime() bool {
	return (p.Policy == schedFIFO || p.Policy == schedRR) && p.RTPrio > 0
}

// parseProcStatLine parses one /proc/<pid>/stat line.
//
// comm is bracketed and may contain spaces AND parentheses — "Audio Main/SPI"
// has the first, "worker (2)" has both — so it is delimited by the LAST ')',
// never tokenised on whitespace.
//
// This is not a hypothetical. Move names six of its threads "Audio Main/SPI".
// Splitting on whitespace shifts every field after comm, and the shifted values
// read as SCHED_OTHER at priority 0 — a clean all-clear from a parser that is
// simply looking in the wrong place. The C original (src/host/rt_thread_audit.c)
// gets this right; keep the two in step.
func parseProcStatLine(line string) (ProcStat, bool) {
	var p ProcStat

	pid, err := strconv.Atoi(strings.TrimSpace(firstToken(line)))
	if err != nil || pid <= 0 {
		return p, false
	}
	p.PID = pid

	open := strings.IndexByte(line, '(')
	close := strings.LastIndexByte(line, ')')
	if open < 0 || close < 0 || close < open {
		return p, false
	}
	p.Comm = line[open+1 : close]

	// Everything after ')' is field 3 onward.
	rest := strings.Fields(line[close+1:])
	field := 2
	var gotCPU, gotPrio, gotPolicy bool
	for _, tok := range rest {
		field++
		switch field {
		case fieldUtime:
			p.Utime, _ = strconv.ParseUint(tok, 10, 64)
		case fieldStime:
			p.Stime, _ = strconv.ParseUint(tok, 10, 64)
		case fieldProcessor:
			p.CPU, _ = strconv.Atoi(tok)
			gotCPU = true
		case fieldRTPrio:
			p.RTPrio, _ = strconv.Atoi(tok)
			gotPrio = true
		case fieldPolicy:
			p.Policy, _ = strconv.Atoi(tok)
			gotPolicy = true
		}
		if gotPolicy {
			break
		}
	}

	// A line that stops before field 41 tells us nothing about scheduling, and
	// reporting it as SCHED_OTHER 0 would be a false all-clear.
	if !gotCPU || !gotPrio || !gotPolicy {
		return p, false
	}
	return p, true
}

func firstToken(s string) string {
	if i := strings.IndexByte(s, ' '); i >= 0 {
		return s[:i]
	}
	return s
}

// CoreStat is one line of /proc/stat.
type CoreStat struct {
	Name  string // "cpu", "cpu0", ...
	Busy  uint64 // jiffies not idle
	Total uint64
}

// parseCPUStat reads the cpu / cpuN lines. Index 0 is the aggregate.
func parseCPUStat(text string) ([]CoreStat, bool) {
	var out []CoreStat
	for _, line := range strings.Split(text, "\n") {
		if !strings.HasPrefix(line, "cpu") {
			continue
		}
		f := strings.Fields(line)
		if len(f) < 5 {
			continue
		}
		var total, idle uint64
		for i, tok := range f[1:] {
			v, err := strconv.ParseUint(tok, 10, 64)
			if err != nil {
				continue
			}
			total += v
			// Fields 4 and 5 (0-based 3 and 4) are idle and iowait.
			if i == 3 || i == 4 {
				idle += v
			}
		}
		out = append(out, CoreStat{Name: f[0], Busy: total - idle, Total: total})
	}
	return out, len(out) > 0
}

// cpuPercent converts a tick delta into a percentage of one core.
//
// It divides by MEASURED elapsed time, never by an assumed poll interval. The
// page polls at ~1 Hz, but a slow response, a second browser, or a
// backgrounded tab all change the real interval — and dividing by the nominal
// one would report a number that is wrong by exactly that factor while looking
// entirely plausible.
func cpuPercent(tickDelta uint64, clkTck int64, elapsed time.Duration) float64 {
	if elapsed <= 0 || clkTck <= 0 {
		return 0
	}
	cpuSeconds := float64(tickDelta) / float64(clkTck)
	return cpuSeconds / elapsed.Seconds() * 100
}

// readProcStat reads /proc/<pid>/stat.
func readProcStat(pid int) (ProcStat, bool) {
	b, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
	if err != nil {
		return ProcStat{}, false
	}
	return parseProcStatLine(strings.TrimSpace(string(b)))
}

// scanProcesses returns every process in /proc.
func scanProcesses() []ProcStat {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var out []ProcStat
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		if p, ok := readProcStat(pid); ok {
			out = append(out, p)
		}
	}
	return out
}

// scanThreads returns every thread of one process, from /proc/<pid>/task.
//
// This is how the module-RT-thread panel is populated. A thread created from a
// module entry point inherits the SPI callback's FIFO priority AND its parent's
// comm, so it reports as "Audio Main/SPI" and is invisible in top or any thread
// list. Nothing here matches on names for that reason — the panel reports
// policy and priority, and lets the reader judge.
func scanThreads(pid int) []ProcStat {
	dir := filepath.Join("/proc", strconv.Itoa(pid), "task")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []ProcStat
	for _, e := range entries {
		b, err := os.ReadFile(filepath.Join(dir, e.Name(), "stat"))
		if err != nil {
			continue
		}
		if t, ok := parseProcStatLine(strings.TrimSpace(string(b))); ok {
			out = append(out, t)
		}
	}
	return out
}

// readLoadAvg returns the three load averages.
func readLoadAvg() (one, five, fifteen float64, ok bool) {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return 0, 0, 0, false
	}
	f := strings.Fields(string(b))
	if len(f) < 3 {
		return 0, 0, 0, false
	}
	one, _ = strconv.ParseFloat(f[0], 64)
	five, _ = strconv.ParseFloat(f[1], 64)
	fifteen, _ = strconv.ParseFloat(f[2], 64)
	return one, five, fifteen, true
}

func readCPUStat() ([]CoreStat, bool) {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return nil, false
	}
	return parseCPUStat(string(b))
}
```

- [ ] **Step 4: Run the tests**

```bash
cd schwung-manager && go test ./... -run TestProc -v 2>&1 | tail -20
```
Expected: all PASS.

- [ ] **Step 5: Prove the comm test can fail**

```bash
cd schwung-manager
sed -i.bak "s/close := strings.LastIndexByte(line, ')')/close := strings.IndexByte(line, ')')/" perf_proc.go
go test ./... -run TestParseProcStatCommWithParen 2>&1 | grep -c FAIL
mv perf_proc.go.bak perf_proc.go
go test ./... -run TestProc 2>&1 | tail -3
```
Expected: FAIL under the mutation (first-`)` truncates `worker (2)` to `worker (2`), then all PASS after restoring.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/perf_proc.go schwung-manager/perf_proc_test.go
git commit -m "manager: /proc parsers for the CPU page

comm is delimited on the LAST ')', matching rt_thread_audit.c. Move names
six threads 'Audio Main/SPI'; splitting on whitespace shifts every field
after comm and reports every one of them as SCHED_OTHER 0 - a clean
all-clear from a parser looking in the wrong place."
```

```json:metadata
{"files": ["schwung-manager/perf_proc.go", "schwung-manager/perf_proc_test.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestProc -v", "acceptanceCriteria": ["parseProcStatLine delimits comm on the last ')' so 'Audio Main/SPI' and 'worker (2)' parse", "a line stopping before field 41 returns ok=false rather than SCHED_OTHER 0", "parseCPUStat returns per-core busy/total from /proc/stat", "cpuPercent divides by measured elapsed, not an assumed interval", "zero elapsed yields 0 rather than infinity", "the comm test fails under a first-paren mutation"], "modelTier": "standard"}
```

---

### Task 6: Join layer, delta state, and the two handlers

**Goal:** `perf.go` builds the view model — frame budget joined to module ids, process table with real deltas, per-core, RT threads — and serves `/system/cpu` and `/system/cpu/values`.

**Files:**
- Create: `schwung-manager/perf.go`
- Create: `schwung-manager/perf_test.go`
- Modify: `schwung-manager/main.go:3568-3574` (register the two routes)

**Acceptance Criteria:**
- [ ] The first `/system/cpu/values` request after arming reports `Priming: true` and no percentages.
- [ ] The second and later requests report percentages computed from the measured interval.
- [ ] A `PerfVersionError` renders as an explicit "deploy the shim and the manager together" message, not as zeros.
- [ ] `ErrPerfAbsent` renders as "shim not running", distinct from an idle device.
- [ ] Slot rows carry the module id from `synth_module` / `fx_module`; Master FX rows from `master_fx:N:module`.
- [ ] Empty slots and unloaded Master FX positions are omitted, not shown at 0.
- [ ] Percentages use `FramePeriodUs` when nonzero, falling back to 2902.
- [ ] `MoveOriginal`, `link-subscriber`, `shadow_ui`, `jackd`, `schwung-manager` appear even at 0%.

**Verify:** `cd schwung-manager && go test ./... -run TestCPU -v && go build ./...` → all PASS, build clean

**Steps:**

- [ ] **Step 1: Write the failing tests**

Create `schwung-manager/perf_test.go`:

```go
package main

import (
	"strings"
	"testing"
	"time"
)

// The first sample has no predecessor, so there is no delta to compute. It must
// say "priming", not 0% — a zero here is indistinguishable from an idle device.
func TestCPUFirstSampleIsPrimingNotZero(t *testing.T) {
	c := &cpuSampler{clkTck: 100}
	view := c.buildProcessView([]ProcStat{
		{PID: 1, Comm: "MoveOriginal", Utime: 500, Stime: 100},
	}, time.Unix(1000, 0))

	if !view.Priming {
		t.Fatal("the first sample must report Priming - a lifetime average " +
			"is not what anyone means by CPU usage, and 0% is a lie")
	}
	if len(view.Rows) != 0 {
		t.Fatal("no rows until there is a delta to report")
	}
}

func TestCPUSecondSampleUsesMeasuredInterval(t *testing.T) {
	c := &cpuSampler{clkTck: 100}
	base := time.Unix(1000, 0)
	c.buildProcessView([]ProcStat{
		{PID: 1, Comm: "MoveOriginal", Utime: 500, Stime: 100},
	}, base)

	// +200 ticks = 2.0s of CPU, over 2s wall = 100% of one core.
	view := c.buildProcessView([]ProcStat{
		{PID: 1, Comm: "MoveOriginal", Utime: 700, Stime: 100},
	}, base.Add(2*time.Second))

	if view.Priming {
		t.Fatal("the second sample has a delta and must not report Priming")
	}
	if len(view.Rows) == 0 {
		t.Fatal("wanted a row for MoveOriginal")
	}
	if view.Rows[0].Percent < 99 || view.Rows[0].Percent > 101 {
		t.Fatalf("Percent = %.2f, want ~100", view.Rows[0].Percent)
	}
}

// Always-listed processes appear even at 0%, so a missing link-subscriber reads
// as absent rather than as silence.
func TestCPUAlwaysListedProcessesAppearAtZero(t *testing.T) {
	c := &cpuSampler{clkTck: 100}
	base := time.Unix(1000, 0)
	procs := []ProcStat{{PID: 1, Comm: "MoveOriginal", Utime: 500}}
	c.buildProcessView(procs, base)
	view := c.buildProcessView(procs, base.Add(time.Second))

	names := map[string]bool{}
	for _, r := range view.Rows {
		names[r.Comm] = true
	}
	for _, want := range alwaysListedProcesses {
		if !names[want] {
			t.Errorf("%q must be listed even at 0%% - absent and idle are "+
				"different sentences", want)
		}
	}
	for _, r := range view.Rows {
		if r.Comm == "link-subscriber" && !r.Absent {
			t.Error("a process that is not running must be marked Absent")
		}
	}
}

// A slot with no module is not a slot at 0% — it is not a row.
func TestCPUFrameBudgetOmitsEmptySlots(t *testing.T) {
	snap := &PerfSnapshot{
		FramePeriodUs: 2902,
		SlotSynthAvg:  [perfChainSlots]uint64{290, 0, 0, 0},
	}
	rows := buildFrameBudget(snap, map[int]string{0: "braids"}, nil)

	if len(rows) != 1 {
		t.Fatalf("got %d rows, want 1 - an empty slot is not a slot at 0%%", len(rows))
	}
	if rows[0].Module != "braids" {
		t.Fatalf("Module = %q, want braids", rows[0].Module)
	}
	// 290 of 2902 us is 10%.
	if rows[0].Percent < 9.5 || rows[0].Percent > 10.5 {
		t.Fatalf("Percent = %.2f, want ~10", rows[0].Percent)
	}
}

// A zero period would divide by zero. Fall back to the nominal 128/44100.
func TestCPUFrameBudgetFallsBackToNominalPeriod(t *testing.T) {
	snap := &PerfSnapshot{FramePeriodUs: 0,
		SlotSynthAvg: [perfChainSlots]uint64{1451, 0, 0, 0}}
	rows := buildFrameBudget(snap, map[int]string{0: "dx7"}, nil)
	if len(rows) != 1 {
		t.Fatalf("got %d rows, want 1", len(rows))
	}
	if rows[0].Percent < 49 || rows[0].Percent > 51 {
		t.Fatalf("Percent = %.2f, want ~50 (1451 of the nominal 2902)", rows[0].Percent)
	}
}

// The deploy-coupling failure must name itself.
func TestCPUVersionMismatchIsExplicit(t *testing.T) {
	msg := describePerfError(&PerfVersionError{Got: 2, Want: 1})
	if !strings.Contains(msg, "deploy the shim and the manager together") {
		t.Fatalf("a version mismatch must say what to do, got %q", msg)
	}
}

func TestCPUAbsentIsNotIdle(t *testing.T) {
	msg := describePerfError(ErrPerfAbsent)
	if !strings.Contains(strings.ToLower(msg), "not running") {
		t.Fatalf("an absent segment must say the shim is not running, got %q", msg)
	}
	if strings.Contains(strings.ToLower(msg), "idle") {
		t.Fatal("'not running' must never be phrased as 'idle'")
	}
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cd schwung-manager && go test ./... -run TestCPU 2>&1 | head -10
```
Expected: FAIL — `undefined: cpuSampler`.

- [ ] **Step 3: Write the implementation**

Create `schwung-manager/perf.go`:

```go
package main

import (
	"errors"
	"fmt"
	"net/http"
	"sort"
	"strconv"
	"sync"
	"time"
)

// The nominal SPI frame period: 128 frames at 44100 Hz. Only a fallback — the
// shim publishes the MEASURED period, and that is what should normally be used.
const nominalFramePeriodUs = 2902

// Below this, a process is noise. The always-listed ones ignore the floor.
const processFloorPercent = 0.5

// Listed even at 0%, so a missing link-subscriber reads as ABSENT rather than
// as silence. "Not running" and "running but idle" are different findings and
// the page must be able to tell them apart.
var alwaysListedProcesses = []string{
	"MoveOriginal", "link-subscriber", "shadow_ui", "jackd", "schwung-manager",
}

// FrameBudgetRow is one entry in the core-3 panel.
type FrameBudgetRow struct {
	Label   string  // "Slot 1 synth", "Master FX 3", "Process MIDI"
	Module  string  // module id, or "" for host sections
	AvgUs   uint64
	MaxUs   uint64
	Percent float64 // avg as a share of the frame period
	MaxPct  float64
	// Note is a caveat shown beside the row. Used for proc_midi, whose cost
	// includes MIDI FX that are NOT separable per module.
	Note string
}

// ProcessRow is one entry in the process panel.
type ProcessRow struct {
	PID     int
	Comm    string
	Percent float64
	Absent  bool // in alwaysListedProcesses but not found in /proc
}

// ProcessView is the process panel.
type ProcessView struct {
	Priming bool // no predecessor sample yet — report nothing, not zero
	Rows    []ProcessRow
}

// cpuSampler holds the one previous sample the delta is computed against.
//
// One sample globally rather than one per client: the page is single-user in
// practice, and because every percentage divides by MEASURED elapsed rather
// than an assumed interval, a second browser shortens the interval without
// making any number wrong.
type cpuSampler struct {
	mu     sync.Mutex
	clkTck int64

	prevProcs map[int]uint64 // pid → utime+stime
	prevAt    time.Time
	prevCores []CoreStat
}

func newCPUSampler() *cpuSampler {
	// USER_HZ is 100 on every Move kernel seen so far, and Go has no portable
	// sysconf(_SC_CLK_TCK). If this is ever wrong, every percentage is wrong by
	// the same factor — which is why the value is named rather than inlined.
	return &cpuSampler{clkTck: 100}
}

func (c *cpuSampler) buildProcessView(procs []ProcStat, now time.Time) ProcessView {
	c.mu.Lock()
	defer c.mu.Unlock()

	cur := make(map[int]uint64, len(procs))
	byPID := make(map[int]ProcStat, len(procs))
	for _, p := range procs {
		cur[p.PID] = p.Utime + p.Stime
		byPID[p.PID] = p
	}

	if c.prevProcs == nil || c.prevAt.IsZero() {
		c.prevProcs, c.prevAt = cur, now
		// A single read of /proc/<pid>/stat is a LIFETIME average, which is not
		// what anyone means by CPU usage. With no predecessor there is no
		// delta, so report priming rather than a number.
		return ProcessView{Priming: true}
	}

	elapsed := now.Sub(c.prevAt)
	var rows []ProcessRow
	seen := map[string]bool{}

	for pid, total := range cur {
		prev, ok := c.prevProcs[pid]
		if !ok || total < prev {
			continue // new process, or pid reused — no honest delta
		}
		pct := cpuPercent(total-prev, c.clkTck, elapsed)
		p := byPID[pid]
		always := false
		for _, n := range alwaysListedProcesses {
			if p.Comm == n {
				always = true
				seen[n] = true
				break
			}
		}
		if pct < processFloorPercent && !always {
			continue
		}
		rows = append(rows, ProcessRow{PID: pid, Comm: p.Comm, Percent: pct})
	}

	for _, n := range alwaysListedProcesses {
		if !seen[n] {
			rows = append(rows, ProcessRow{Comm: n, Absent: true})
		}
	}

	sort.Slice(rows, func(i, j int) bool { return rows[i].Percent > rows[j].Percent })

	c.prevProcs, c.prevAt = cur, now
	return ProcessView{Rows: rows}
}

// buildFrameBudget turns a snapshot into ranked rows.
//
// slotModules maps slot index → module id; mfxModules maps Master FX position →
// module id. A position with no module produces NO ROW — it is not a slot at
// 0%, it is not a slot.
func buildFrameBudget(snap *PerfSnapshot, slotModules, mfxModules map[int]string) []FrameBudgetRow {
	if snap == nil {
		return nil
	}
	period := float64(snap.FramePeriodUs)
	if period <= 0 {
		period = nominalFramePeriodUs
	}
	pct := func(us uint64) float64 { return float64(us) / period * 100 }

	var rows []FrameBudgetRow
	add := func(label, module string, avg, max uint64, note string) {
		rows = append(rows, FrameBudgetRow{
			Label: label, Module: module,
			AvgUs: avg, MaxUs: max,
			Percent: pct(avg), MaxPct: pct(max),
			Note: note,
		})
	}

	for i := 0; i < perfChainSlots; i++ {
		mod, ok := slotModules[i]
		if !ok || mod == "" {
			continue
		}
		if snap.SlotSynthAvg[i] > 0 || snap.SlotSynthMax[i] > 0 {
			add(fmt.Sprintf("Slot %d synth", i+1), mod,
				snap.SlotSynthAvg[i], snap.SlotSynthMax[i], "")
		}
		if snap.SlotFxAvg[i] > 0 || snap.SlotFxMax[i] > 0 {
			add(fmt.Sprintf("Slot %d FX", i+1), mod,
				snap.SlotFxAvg[i], snap.SlotFxMax[i], "")
		}
	}

	for i := 0; i < perfMasterFXSlots; i++ {
		mod, ok := mfxModules[i]
		if !ok || mod == "" {
			continue
		}
		add(fmt.Sprintf("Master FX %d", i+1), mod, snap.MfxAvg[i], snap.MfxMax[i], "")
	}

	if snap.OvertakeGenAvg > 0 || snap.OvertakeGenMax > 0 {
		add("Overtake generator", "", snap.OvertakeGenAvg, snap.OvertakeGenMax, "")
	}
	if snap.OvertakeFxAvg > 0 || snap.OvertakeFxMax > 0 {
		add("Overtake FX", "", snap.OvertakeFxAvg, snap.OvertakeFxMax, "")
	}

	for i, name := range perfSectionNames {
		if i >= perfSectionCount {
			break
		}
		if snap.SectionAvg[i] == 0 && snap.SectionMax[i] == 0 {
			continue
		}
		note := ""
		if name == "Process MIDI (incl. MIDI FX)" {
			note = "MIDI FX run event-driven inside the chain host, " +
				"so their cost lands here and is not separable per module."
		}
		add(name, "", snap.SectionAvg[i], snap.SectionMax[i], note)
	}

	sort.SliceStable(rows, func(i, j int) bool { return rows[i].AvgUs > rows[j].AvgUs })
	return rows
}

// describePerfError turns a read failure into a sentence the page can show.
//
// Every branch here says something DIFFERENT. A failed read must never be
// rendered as an idle device: "the shim is not running", "the shim is older
// than this manager" and "everything is idle" are three findings, and a 0% bar
// would tell the same lie for all three.
func describePerfError(err error) string {
	var ve *PerfVersionError
	switch {
	case err == nil:
		return ""
	case errors.As(err, &ve):
		return ve.Error()
	case errors.Is(err, ErrPerfAbsent):
		return "The shim is not running, or this is not a Move. " +
			"No frame-budget data is available — this is not the same as an idle device."
	case errors.Is(err, ErrPerfMagic):
		return "/schwung-perf holds something unexpected. Restart the shim so the segment is recreated."
	case errors.Is(err, ErrPerfTorn):
		return "The snapshot did not settle across three reads. Try again in a moment."
	default:
		return "Frame-budget read failed: " + err.Error()
	}
}

// slotModuleIDs asks the shim which module sits in each chain slot.
func (app *App) slotModuleIDs() map[int]string {
	out := map[int]string{}
	shm := app.shmParams
	if shm == nil {
		return out
	}
	for slot := 0; slot < perfChainSlots; slot++ {
		if id, ok, err := shm.TryGetParam(slot, "synth_module"); ok && err == nil && id != "" {
			out[slot] = id
		}
	}
	return out
}

// mfxModuleIDs asks the shim which module sits in each Master FX position.
func (app *App) mfxModuleIDs() map[int]string {
	out := map[int]string{}
	shm := app.shmParams
	if shm == nil {
		return out
	}
	for i := 0; i < perfMasterFXSlots; i++ {
		key := "master_fx:" + strconv.Itoa(i) + ":module"
		if id, ok, err := shm.TryGetParam(0, key); ok && err == nil && id != "" {
			out[i] = id
		}
	}
	return out
}

// handleSystemCPU renders the idle page. Nothing is sampled until the user
// presses Measure CPU.
func (app *App) handleSystemCPU(w http.ResponseWriter, r *http.Request) {
	app.render(w, r, "system_cpu.html", map[string]any{
		"Title":           "CPU",
		"Active":          "system",
		"FramePeriodUs":   nominalFramePeriodUs,
		"PollIntervalSec": 1,
	})
}

// handleSystemCPUValues is the polled partial. Polling lives entirely in the
// browser (hx-trigger on the returned fragment), so closing the tab stops it —
// there is no server-side session, timer or goroutine to leak.
func (app *App) handleSystemCPUValues(w http.ResponseWriter, r *http.Request) {
	now := time.Now()

	var snap *PerfSnapshot
	var perfErr error
	if app.perfShm != nil {
		snap, perfErr = app.perfShm.Read()
	} else {
		perfErr = ErrPerfAbsent
	}

	var budget []FrameBudgetRow
	if snap != nil {
		budget = buildFrameBudget(snap, app.slotModuleIDs(), app.mfxModuleIDs())
	}

	procView := app.cpuSampler.buildProcessView(scanProcesses(), now)
	cores, _ := readCPUStat()
	one, five, fifteen, _ := readLoadAvg()

	var threads []ProcStat
	for _, p := range scanProcesses() {
		if p.Comm == "MoveOriginal" {
			for _, t := range scanThreads(p.PID) {
				if t.IsRealtime() {
					threads = append(threads, t)
				}
			}
			break
		}
	}
	sort.Slice(threads, func(i, j int) bool {
		return threads[i].RTPrio > threads[j].RTPrio
	})

	app.renderPartial(w, r, "system_cpu.html", "cpu_values", map[string]any{
		"Budget":       budget,
		"PerfError":    describePerfError(perfErr),
		"Snapshot":     snap,
		"Process":      procView,
		"Cores":        cores,
		"Load1":        one,
		"Load5":        five,
		"Load15":       fifteen,
		"RTThreads":    threads,
		"SPICore":      3,
		"Measuring":    true,
	})
}

// handleSystemCPUIdle serves the not-sampling fragment the Stop button swaps
// in. It also RESETS the delta baseline: a stale predecessor from before the
// pause would make the first sample after resuming average across the whole
// gap — a plausible-looking wrong number, which is the worst kind.
func (app *App) handleSystemCPUIdle(w http.ResponseWriter, r *http.Request) {
	app.cpuSampler.reset()
	app.renderPartial(w, r, "system_cpu.html", "cpu_idle", map[string]any{
		"FramePeriodUs":   nominalFramePeriodUs,
		"PollIntervalSec": 1,
	})
}
```

And the reset, beside `buildProcessView`:

```go
func (c *cpuSampler) reset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.prevProcs = nil
	c.prevAt = time.Time{}
}
```

**`renderPartial` does not exist yet — add it.** `handleSystemLogs` writes plain text, so there is no precedent to copy. `loadTemplates()` parses `templates/partials/*.html` into *every* page clone, so a partial can be executed by name off any page template. Add to `main.go` beside `render`:

```go
// renderPartial writes one {{define}} block without the base layout, for htmx
// fragment swaps.
//
// loadTemplates parses templates/partials/*.html into every page's clone, so
// any page template can execute any partial by name. system_cpu.html is used
// as the host purely because it is the page these fragments belong to.
func (app *App) renderPartial(w http.ResponseWriter, r *http.Request, host, name string, data map[string]any) {
	t, ok := app.tmpl[host]
	if !ok {
		app.logger.Error("template not found", "template", host)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
		return
	}
	if cookie, err := r.Cookie("csrf_token"); err == nil {
		data["CSRFToken"] = cookie.Value
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := t.ExecuteTemplate(w, name, data); err != nil {
		app.logger.Error("partial render", "partial", name, "err", err)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
	}
}
```

The two call sites in `perf.go` then read:

```go
	app.renderPartial(w, r, "system_cpu.html", "cpu_values", map[string]any{ ... })
	app.renderPartial(w, r, "system_cpu.html", "cpu_idle", map[string]any{ ... })
```

- [ ] **Step 4: Wire the App fields, the template, and the routes**

In `main.go`, add to the `App` struct (beside `shmParams`, ~line 795):

```go
	perfShm    *PerfShm    // /schwung-perf frame budget (nil if not on device)
	cpuSampler *cpuSampler // holds the previous /proc sample for the delta
```

In the App constructor, beside where `shmParams` is opened:

```go
	app.perfShm = OpenPerfShm()      // nil when not on device
	app.cpuSampler = newCPUSampler()
```

**Register the new page template.** In `loadTemplates()`, add to the `pages` slice — without this the page 404s at render time with "template not found", and `templates_test.go` will not catch it because it only asserts two specific names:

```go
		"templates/system.html",
		"templates/system_cpu.html",
```

Register the routes beside the other `/system` ones (after line 3574):

```go
	mux.HandleFunc("GET /system/cpu", app.handleSystemCPU)
	mux.HandleFunc("GET /system/cpu/values", app.handleSystemCPUValues)
	mux.HandleFunc("GET /system/cpu/idle", app.handleSystemCPUIdle)
```

Extend `templates_test.go`'s `required` list so a future edit that drops the page from `loadTemplates` fails a test rather than 404ing in the field:

```go
	required := []string{
		"config.html",
		"module_detail.html",
		"system_cpu.html",
	}
```

- [ ] **Step 5: Run the tests and build**

```bash
cd schwung-manager && go test ./... -run TestCPU -v 2>&1 | tail -20 && go build ./... && go vet ./...
```
Expected: all PASS, clean build, no vet complaints.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/perf.go schwung-manager/perf_test.go schwung-manager/main.go
git commit -m "manager: CPU view model and handlers

Delta math divides by measured elapsed, so a slow poll or a second browser
yields a correct number rather than a scaled-wrong one. The first sample
reports priming, never 0%."
```

```json:metadata
{"files": ["schwung-manager/perf.go", "schwung-manager/perf_test.go", "schwung-manager/main.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestCPU -v && go build ./... && go vet ./...", "acceptanceCriteria": ["first values request reports Priming and no percentages", "later requests compute from the measured interval", "PerfVersionError renders an explicit deploy-both message", "ErrPerfAbsent renders as shim-not-running, never as idle", "slot rows carry synth_module ids and Master FX rows master_fx:N:module ids", "empty slots and unloaded Master FX positions produce no row", "percentages use FramePeriodUs when nonzero, else 2902", "the five always-listed processes appear even at 0% and are marked Absent when missing"], "modelTier": "standard"}
```

---

### Task 7: Templates and styles

**Goal:** The page renders: idle with a [Measure CPU] button, polling on click, [Stop] to end, four panels, accessible.

**Files:**
- Create: `schwung-manager/templates/system_cpu.html`
- Create: `schwung-manager/templates/partials/cpu_values.html`
- Create: `schwung-manager/templates/partials/cpu_idle.html`
- Modify: `schwung-manager/static/style.css`
- Modify: `schwung-manager/templates/system.html` (link to the new page)

**Acceptance Criteria:**
- [ ] `/system/cpu` renders idle with no sampling.
- [ ] [Measure CPU] swaps in a partial carrying `hx-trigger="every 1s"`; [Stop] swaps back to a partial with none.
- [ ] Frame budget and process CPU are under separate headings that state which cores each covers.
- [ ] The `proc_midi` row shows its "not separable per module" note.
- [ ] Every bar has `role="progressbar"` with `aria-valuenow` and an `aria-label`, and every table reads correctly without the bars.
- [ ] `PerfError` renders as a message in place of the frame-budget table, never as empty rows.
- [ ] `Priming` renders as "priming…" in place of the process table.
- [ ] `templates_test.go` still passes (it parses every template).

**Verify:** `cd schwung-manager && go test ./... && go build ./...` → PASS, then `go run . -base /tmp/fake` and load `http://localhost:7700/system/cpu`

**Steps:**

- [ ] **Step 1: Read the existing conventions**

```bash
sed -n '1,40p' schwung-manager/templates/partials/flash.html
grep -n "info-card\|progress-bar\|progress-fill" schwung-manager/static/style.css | head -20
sed -n '1,60p' schwung-manager/templates_test.go
```

Match whatever `templates_test.go` requires of a new template (it parses all of them; a `{{define}}` name mismatch fails there first).

- [ ] **Step 2: Write `system_cpu.html`**

```html
{{template "base" .}}

{{define "title"}}CPU - Schwung Manager{{end}}

{{define "content"}}
<div class="page-header">
    <h1>CPU</h1>
</div>

<section class="info-card">
    <h2>About these numbers</h2>
    <p>
        Two different quantities, which must not be added together.
    </p>
    <dl class="detail-list">
        <dt>Frame budget (core 3)</dt>
        <dd>
            Per-module time as a share of the {{.FramePeriodUs}}&nbsp;µs SPI frame period.
            This is the number that decides whether you get dropouts. Modules are not
            processes — every synth and effect runs inside MoveOriginal on the SPI
            callback — so this is the only per-module attribution that exists.
        </dd>
        <dt>Process CPU (cores 0–3)</dt>
        <dd>
            Ordinary per-process percentages from <code>/proc</code>. The Link Audio
            subscriber is a real process and appears here.
        </dd>
    </dl>
    <p class="muted">
        Measuring polls once a second while this page is open. Collection in the
        shim is always on and costs nothing measurable; the polling is what this
        button starts.
    </p>
</section>

<div id="cpu-panel">
    {{template "cpu_idle" .}}
</div>
{{end}}
```

- [ ] **Step 3: Write `partials/cpu_idle.html`**

```html
{{define "cpu_idle"}}
<section class="info-card">
    <button class="btn btn-primary"
            hx-get="/system/cpu/values"
            hx-target="#cpu-panel"
            hx-swap="innerHTML">
        Measure CPU
    </button>
    <p class="muted">Not currently sampling.</p>
</section>
{{end}}
```

- [ ] **Step 4: Write `partials/cpu_values.html`**

```html
{{define "cpu_values"}}
<div hx-get="/system/cpu/values"
     hx-trigger="every 1s"
     hx-target="#cpu-panel"
     hx-swap="innerHTML">

    <section class="info-card">
        <button class="btn btn-secondary"
                hx-get="/system/cpu/idle"
                hx-target="#cpu-panel"
                hx-swap="innerHTML">
            Stop
        </button>
        <span class="muted" role="status">Sampling once a second.</span>
    </section>

    <section class="info-card">
        <h2>Frame budget — core 3 (SPI callback)</h2>
        {{if .PerfError}}
            <p class="notice notice-warn" role="alert">{{.PerfError}}</p>
        {{else if not .Budget}}
            <p class="muted">No slots or effects loaded.</p>
        {{else}}
            <p class="muted">
                Averaged over {{.Snapshot.SampleWindowFrames}} frames.
                Max is a spike, not a load.
            </p>
            <table class="data-table">
                <thead>
                    <tr><th>What</th><th>Module</th><th>Avg</th><th>% frame</th><th>Max</th></tr>
                </thead>
                <tbody>
                {{range .Budget}}
                    <tr>
                        <td>
                            {{.Label}}
                            {{if .Note}}<br><small class="muted">{{.Note}}</small>{{end}}
                        </td>
                        <td>{{if .Module}}{{.Module}}{{else}}<span class="muted">host</span>{{end}}</td>
                        <td>{{.AvgUs}} µs</td>
                        <td>
                            <div class="progress-bar" role="progressbar"
                                 aria-valuenow="{{printf "%.0f" .Percent}}"
                                 aria-valuemin="0" aria-valuemax="100"
                                 aria-label="{{.Label}}: {{printf "%.1f" .Percent}} percent of the SPI frame">
                                <div class="progress-fill{{if gt .Percent 40.0}} progress-danger{{else if gt .Percent 20.0}} progress-warning{{end}}"
                                     style="width: {{printf "%.1f" .Percent}}%">
                                    {{printf "%.1f" .Percent}}%
                                </div>
                            </div>
                        </td>
                        <td>{{.MaxUs}} µs</td>
                    </tr>
                {{end}}
                </tbody>
            </table>
        {{end}}
    </section>

    <section class="info-card">
        <h2>Processes — cores 0–3</h2>
        {{if .Process.Priming}}
            <p class="muted" role="status">
                Priming — a single reading of <code>/proc</code> is a lifetime
                average, so the first percentage arrives with the next sample.
            </p>
        {{else}}
            <table class="data-table">
                <thead><tr><th>Process</th><th>PID</th><th>CPU</th></tr></thead>
                <tbody>
                {{range .Process.Rows}}
                    <tr>
                        <td>{{.Comm}}</td>
                        <td>{{if .Absent}}<span class="muted">—</span>{{else}}{{.PID}}{{end}}</td>
                        <td>
                            {{if .Absent}}
                                <span class="muted">not running</span>
                            {{else}}
                                {{printf "%.1f" .Percent}}%
                            {{end}}
                        </td>
                    </tr>
                {{end}}
                </tbody>
            </table>
        {{end}}
    </section>

    <section class="info-card">
        <h2>Cores</h2>
        <p class="muted">Load average: {{printf "%.2f" .Load1}} / {{printf "%.2f" .Load5}} / {{printf "%.2f" .Load15}}</p>
        <table class="data-table">
            <thead><tr><th>Core</th><th>Busy jiffies</th><th>Total</th></tr></thead>
            <tbody>
            {{range .Cores}}
                <tr>
                    <td>{{.Name}}{{if eq .Name "cpu3"}} <span class="muted">(SPI)</span>{{end}}</td>
                    <td>{{.Busy}}</td>
                    <td>{{.Total}}</td>
                </tr>
            {{end}}
            </tbody>
        </table>
    </section>

    <section class="info-card">
        <h2>Realtime threads inside MoveOriginal</h2>
        <p class="muted">
            A thread created from a module entry point inherits the SPI callback's
            priority and its parent's name, so it reports as
            <code>Audio Main/SPI</code> and is invisible in a normal thread list.
            Nothing here matches on names.
        </p>
        {{if not .RTThreads}}
            <p class="muted">None found.</p>
        {{else}}
            <table class="data-table">
                <thead><tr><th>TID</th><th>Name</th><th>Policy</th><th>Priority</th><th>Core</th></tr></thead>
                <tbody>
                {{range .RTThreads}}
                    <tr>
                        <td>{{.PID}}</td>
                        <td>{{.Comm}}</td>
                        <td>{{if eq .Policy 1}}FIFO{{else if eq .Policy 2}}RR{{else}}OTHER{{end}}</td>
                        <td>{{.RTPrio}}</td>
                        <td>{{.CPU}}</td>
                    </tr>
                {{end}}
                </tbody>
            </table>
        {{end}}
    </section>
</div>
{{end}}
```

- [ ] **Step 5: Link it from the System page**

`handleSystemCPUIdle`, its route and `cpuSampler.reset()` were all added in Task 6 — nothing to do here for the Stop button beyond the `cpu_idle.html` partial written in Step 3.

In `schwung-manager/templates/system.html`, add a card after the Disk Usage section:

```html
    <section class="info-card">
        <h2>CPU</h2>
        <p>See where the device's CPU goes, by module and by process.</p>
        <a class="btn btn-secondary" href="/system/cpu">Open CPU view</a>
    </section>
```

- [ ] **Step 6: Add the styles**

`.progress-bar`, `.progress-fill`, `.progress-warning`, `.progress-danger` and `.info-card` already exist (the Disk Usage card uses them) and are reused as-is. `.data-table`, `.muted` and `.notice-warn` do **not** exist and are new. Append to `schwung-manager/static/style.css`:

```css
/* CPU page. The progress bars are DECORATION: every row states its own
   numbers in adjacent cells, so the table reads correctly with images off,
   with CSS off, and through a screen reader. */
.data-table { width: 100%; border-collapse: collapse; }
.data-table th,
.data-table td { padding: 0.4rem 0.6rem; text-align: left; vertical-align: middle; }
.data-table thead th { border-bottom: 1px solid currentColor; opacity: 0.6; font-weight: 600; }
.data-table tbody tr + tr td { border-top: 1px solid rgba(128, 128, 128, 0.2); }
.data-table .progress-bar { min-width: 8rem; margin: 0; }

.notice-warn {
    padding: 0.75rem 1rem;
    border-left: 3px solid #b58900;
    background: rgba(181, 137, 0, 0.08);
}

.muted { opacity: 0.7; }
```

Confirm none of the three were added by another branch in the meantime:

```bash
grep -c "^\.muted\|^\.data-table\|^\.notice-warn" schwung-manager/static/style.css
```
Expected before the edit: `0`. If it is not, merge rather than duplicate — a second definition later in the file silently wins.

- [ ] **Step 7: Verify**

```bash
cd schwung-manager && go test ./... 2>&1 | tail -5 && go build ./...
```
Expected: all PASS (including `templates_test.go`, which parses every template), clean build.

Then run it locally and load the page. It will show "the shim is not running" for the frame budget, which is the correct answer on a dev machine — and is the thing to confirm, because it proves absent is not being rendered as idle:

```bash
cd schwung-manager && go run . -base /tmp/fake-schwung
```
Open `http://localhost:7700/system/cpu`, press **Measure CPU**, confirm:
- the frame-budget panel shows the "shim is not running" message, not an empty table;
- the process panel shows "Priming" on the first tick and real percentages from the second;
- **Stop** halts the polling (watch the terminal's request log go quiet).

- [ ] **Step 8: Commit**

```bash
git add schwung-manager/templates/ schwung-manager/static/style.css schwung-manager/main.go schwung-manager/perf.go
git commit -m "manager: CPU page templates

Frame budget and process CPU sit under separate headings naming the cores
each covers - they are different quantities and adding them would be
meaningless. Polling lives in the returned fragment's hx-trigger, so
closing the tab stops it and there is no server-side state to leak."
```

```json:metadata
{"files": ["schwung-manager/templates/system_cpu.html", "schwung-manager/templates/partials/cpu_values.html", "schwung-manager/templates/partials/cpu_idle.html", "schwung-manager/static/style.css", "schwung-manager/templates/system.html", "schwung-manager/main.go", "schwung-manager/perf.go"], "verifyCommand": "cd schwung-manager && go test ./... && go build ./...", "acceptanceCriteria": ["/system/cpu renders idle with no sampling", "Measure CPU swaps in a partial with hx-trigger every 1s and Stop swaps back to one without", "frame budget and process CPU are under separate headings naming their cores", "the proc_midi row shows its not-separable note", "every bar has role=progressbar with aria-valuenow and aria-label and the tables read without bars", "PerfError renders as a message in place of the table, never as empty rows", "Priming renders in place of the process table", "templates_test.go passes"], "modelTier": "standard"}
```

---

### Task 8: Documentation

**Goal:** `docs/DIAGNOSTICS.md` gains the section; `CLAUDE.md` gains one bullet under its Diagnostics hook, not the prose.

**Files:**
- Modify: `docs/DIAGNOSTICS.md`
- Modify: `CLAUDE.md`

**Acceptance Criteria:**
- [ ] `docs/DIAGNOSTICS.md` describes `/schwung-perf`, what the two panels mean, and why they must not be added together.
- [ ] It states that this one is **always on**, unlike every other switch in that file, and why.
- [ ] It records that MIDI FX are not separable and land in `proc_midi`.
- [ ] `CLAUDE.md` gains **one bullet** under the existing `docs/DIAGNOSTICS.md` hook — no prose inlined.
- [ ] `tests/host/test_perf_shm_offsets.sh` and `test_perf_snapshot_size` are named in the docs so the next person finds them.

**Verify:** `grep -c "schwung-perf" docs/DIAGNOSTICS.md CLAUDE.md` → both non-zero; `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done | grep perf` → no output

**Steps:**

- [ ] **Step 1: Add the DIAGNOSTICS section**

Append to `docs/DIAGNOSTICS.md`:

```markdown
## The CPU page (`/system/cpu`) — and the one diagnostic that is always on

Every other switch in this file is off by default. This one is not, and the
reason is worth writing down: the timing it publishes was **already being
collected unconditionally**. `SHADOW_TIMING_LOG` is `0`, but it gates only the
`fopen`/`fprintf` — all 35 `clock_gettime` sites and all 25 `TIME_SECTION_START`
macros in `src/schwung_shim.c` compile in and run on every frame regardless.
Publishing them costs one `mmap` and two stores per ~1000 frames, because
`spi_snap` is a POINTER into `/schwung-perf` rather than a static that gets
copied there.

Arming it would have made the page blank by default — the failure mode the SPI
tally's 20-second silence already teaches. What the page's **Measure CPU**
button arms is the manager's 1 Hz `/proc` polling, which is the only part with a
real cost, and which runs `SCHED_OTHER` on cores 0–2 and never touches core 3.

### The two numbers are not addable

- **Frame budget (core 3).** Per-module µs as a share of the SPI frame period.
  **Modules are not processes** — every slot synth, slot FX, Master FX and
  overtake DSP is a `.so` running on the SPI callback inside MoveOriginal — so
  `/proc` can report what MoveOriginal costs in total and can never split it by
  module. This panel is the only per-module attribution that exists.
- **Process CPU (cores 0–3).** Ordinary `/proc` percentages. `link-subscriber`
  is a real process and appears here.

Adding them would double-count everything the shim does, which is why they sit
under separate headings that name their cores.

### What is NOT separable

**MIDI FX have no per-frame render.** They run event-driven inside the chain
host's `on_midi`, so their cost lands in the `proc_midi` section and is
attributed to no module. The page labels that row rather than inventing a
number for it.

### Averages, not just maxima

The per-slot entries were **max-only** before this page existed, which is a
spike detector rather than a load figure. `slot_synth_avg[]`, `slot_fx_avg[]`,
`mfx_avg[]` and the overtake pair are new. The denominator is
`frame_period_us`, which is `frame_total_avg` — the **measured** period, not the
2902 µs nominal — so a percentage cannot drift from reality if the period
changes. (Remember `total_us` is not a load signal on its own: our work shrinks
the driver's wait by the same amount.)

### The segment

`/schwung-perf`, `schwung_perf_snapshot_t` in `src/host/perf_snapshot.h`, v1,
one page. `magic` and `version` are the first two fields so a short segment left
by an older shim can be version-checked without touching its tail. A seqlock on
`seq` (odd = writing) makes cross-process reads tear-free; a reader that cannot
settle after three attempts reports **failed**, never zeros.

**Shim and manager ship together.** A version mismatch shows an explicit
"deploy both" message on the page rather than rendering garbage.

Two tests hold this together, and the second is the one that matters:

- `tests/host/test_perf_snapshot_size.c` — the container can only grow.
- `tests/host/test_perf_shm_offsets.sh` — compiles a probe that prints
  `offsetof` for every field and diffs it against the Go const block in
  `schwung-manager/perf_shm.go`. `shmparams.go` carries the same kind of
  hand-mirrored layout, and when two `uint64`s were added to `shadow_param_t`
  the Go side was not updated: every key landed 16 bytes late, the shim read an
  empty key, and every GET returned empty — with nothing crashing and no test
  failing. Drift in a hand-mirrored layout is silent and total.
```

- [ ] **Step 2: Add the CLAUDE.md bullet**

In `CLAUDE.md`, under the existing `**Device diagnostics — docs/DIAGNOSTICS.md**` bullet list (after the "silent for ~20 s after arming" line), add:

```markdown
- **The CPU page is the one diagnostic that is ALWAYS ON**, because its timing
  was already collected unconditionally — `SHADOW_TIMING_LOG` gates only the
  `fprintf`, so 35 `clock_gettime` sites run every frame regardless, and
  `spi_snap` is now a POINTER into `/schwung-perf` rather than a static copied
  there. **Modules are not processes**, so `/proc` can never split MoveOriginal
  by module and the frame-budget panel is the only per-module attribution that
  exists — it must never be added to the process panel. MIDI FX are **not
  separable** (no per-frame render; they land in `proc_midi`).
```

- [ ] **Step 3: Verify**

```bash
grep -c "schwung-perf" docs/DIAGNOSTICS.md CLAUDE.md
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
make -C tests/host test 2>&1 | tail -5
cd schwung-manager && go test ./... 2>&1 | tail -3
```
Expected: both counts non-zero; no `FAIL` lines mentioning `perf`; host tests green; Go tests green.

> Pre-existing failures in `tests/{shadow,store,build}` are expected — CI gates only `tests/host/`. Do not chase them.

- [ ] **Step 4: Commit and open the PR**

```bash
git add docs/DIAGNOSTICS.md CLAUDE.md
git commit -m "docs: the CPU page, and why it is the one always-on diagnostic"
git push -u origin manager-cpu-view
gh pr create --title "Manager: CPU usage view, by module and by process" --body "$(cat <<'PREOF'
Adds `/system/cpu` to schwung-manager.

**Modules are not processes.** Every slot synth, slot FX, Master FX and
overtake DSP is a `.so` running on MoveOriginal's SPI callback, so `/proc`
can say what MoveOriginal costs in total but never split it by module.
Per-module CPU therefore comes from the shim's timing, published to a new
`/schwung-perf` segment; the process table comes from `/proc`. The page
keeps them under separate headings naming the cores each covers, because
adding them would double-count.

- Per-slot timing gains **averages** — the existing entries were max-only,
  which is a spike detector, not a load figure.
- Master FX and the overtake DSP are timed for the first time.
- Collection is always on (the `clock_gettime` calls were already
  unconditional; `SHADOW_TIMING_LOG` gates only the `fprintf`). The page's
  **Measure CPU** button arms the 1 Hz polling, which is the only part with
  a real cost and runs off the RT path.
- A read that fails reports **failed** — "shim not running", "shim is older
  than this manager" and "everything idle" are three findings, and a 0% bar
  would tell the same lie for all three.

**Shim and manager must be deployed together.** Nothing here has been
deployed.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01GusidCoKfCDebmjTSQ2wdF
PREOF
)"
```

```json:metadata
{"files": ["docs/DIAGNOSTICS.md", "CLAUDE.md"], "verifyCommand": "grep -c 'schwung-perf' docs/DIAGNOSTICS.md CLAUDE.md && make -C tests/host test 2>&1 | tail -5", "acceptanceCriteria": ["DIAGNOSTICS.md describes /schwung-perf and why the two panels are not addable", "it states this diagnostic is always on, unlike every other switch there, and why", "it records that MIDI FX are not separable and land in proc_midi", "CLAUDE.md gains exactly one bullet under the DIAGNOSTICS hook with no prose inlined", "both new tests are named in the docs"], "modelTier": "standard"}
```

---

## Deploy

**Nothing in this plan deploys.** Per the user's instruction, the branch is built and tested but never installed. When the time comes it is a single coupled deploy, because the manager's frame-budget panel needs the new shim:

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Until then the page renders "the shim is not running" for the frame budget, which is the correct and honest answer.
