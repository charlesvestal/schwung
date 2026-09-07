# Slot Mod Routes Implementation Plan

> **For agentic workers:** Use `superpowers-extended-cc:subagent-driven-development` or
> `superpowers-extended-cc:executing-plans` to implement this task-by-task.

**Goal:** Turn the slot's two LFOs into eight general **mod routes**, each with a
selectable source — LFO, Velocity, Pressure, CC, or Note — aimed at any parameter of any
component in the slot.

**Architecture:** The chain host already carries a source-agnostic modulation bus
(`chain_mod.c`: `chain_mod_emit_value(ctx, source_id, target, param, signal, depth,
offset, bipolar, enabled)` — 32 targets × 8 sources, non-destructive base/effective
overlay, range-scaled by the target's declared `chain_params` min/max). Only the two
LFOs are wired into it. This plan generalises the **producer**: `lfo_state_t` gains a
`src` discriminator, the count goes 2 → 8, and a new pure header turns latched MIDI into
the same bipolar `signal` an LFO produces. Everything downstream of `signal` — depth,
polarity, offset, range scaling, the emit call, the target picker, the permutation
remap — is untouched.

**Tech Stack:** C11 header-only pure units under `src/host/` (compiled and RUN natively
by `tests/host`), ES modules for the shadow UI, `tests/host/*.sh` source pins.

**User decisions (already made):**
- **"Rename to mod routes, alias the old keys."** New keys `mod1:`..`mod8:`; the patch
  parser keeps reading the legacy `"lfos": {"lfo1": …}` section so existing sets load.
- **"8"** routes per slot — matching `MAX_AUDIO_FX` / `SLOT_BUSES` / `MASTER_FX_SLOTS`.
- **Sources:** Velocity, Pressure, CC (any number), Note/keytrack, alongside the LFO.
- **"8mb of ram is fine, we have lots."** Verified: `mod_routes` lives in
  `chain_instance_t` (heap). `patch_info_t` — which IS a stack local on `v2_set_param`'s
  `load_file` route, a 232 KB frame — grows by only ~780 bytes.

**Explicitly out of scope** (deliberate, discussed): bus-insert FX (`bus<N>:fx<M>`) and
global send FX as mod *targets*; cross-slot modulation. Both are real follow-ups — see
"Deferred" at the end.

---

## Context before Task 0

Read these. Each is short and load-bearing:

- `src/host/lfo_common.h` — shared by the chain host (slot LFOs) **and**
  `src/host/shadow_chain_mgmt.c` (Master FX LFOs). **Master FX is out of scope and must
  stay at 2 routes.**
- `src/modules/chain/dsp/chain_mod.c` — the bus. **Do not change it in this plan.** Its
  contract is what makes everything else safe.
- `docs/CHAIN.md`, `docs/PARAM_PAGES.md`, `CLAUDE.md` § "Realtime Safety".

Three rules govern every task:

1. **Every entry point here IS the SPI callback.** `create_instance`, `set_param`,
   `get_param`, `on_midi`, `render_block`. No allocation, no file I/O, no logging on any
   path a route touches. `chain_record_synth_note` is the model: an int store, nothing else.
2. **A modulated value must never be written back as a base.** `chain_mod_emit_value`
   handles this for plugin params. Send amounts do *not* go through it — they accumulate
   into `main_send_mod[]` precisely because `saveSendLevels()` reads the level back
   (`chain_host.c:2265`). Check any new target class against that question.
3. **Names must not drift.** A half-rename is worse than either end state.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/host/lfo_common.h` | waveform/division maths; `lfo_process_midi` gains a count | 0, 2 |
| `src/host/mod_src.h` | **new.** Source enum, names, latched-MIDI → `signal` arithmetic | 1 |
| `chain_internal.h` | `MOD_ROUTE_COUNT`, `mod_routes[]`, `mod_input` | 2 |
| `chain_host.c` | `modN:`/legacy `lfoN:` ladders; `mod_tick` dispatch | 2, 4 |
| `chain_midi.c` | latch MIDI at both synth-feed paths | 3 |
| `chain_patch.c` | read legacy `"lfos"`, read/write `"mod_routes"` | 5 |
| `chain_reorder.c` | permutation remap over 8 routes | 6 |
| `shadow_ui_slot_grid.mjs` | `modParams`/`modLevels`; the `src` cell and its gating | 7 |
| `shadow_ui.js`, `lfo_target_label.mjs` | picker, labels, mod-to-mod range | 8 |
| docs + `help_content.json` | the war stories, one `CLAUDE.md` bullet each | 10 |

---

### Task 0: `lfo_process_midi` takes an explicit count

**Goal:** Remove the latent buffer overrun that raising the route count would arm.

**Why now:** `lfo_process_midi(lfo_state_t *lfos, const uint8_t *msg, int len)` takes a
pointer but loops the **global** `LFO_COUNT`. Today `MASTER_FX_LFO_COUNT == LFO_COUNT ==
2`, so it is harmless and invisible. The moment Task 2 takes slot routes to 8 while
Master FX stays at 2, a call with `shadow_master_fx_lfos` walks six elements off the end
— on the SPI callback. Fix it before the count moves, not after.

**Files:**
- Modify: `src/host/lfo_common.h:58-77`, `src/modules/chain/dsp/chain_midi.c:758`
- Create: `tests/host/test_lfo_process_midi_count.c`, `.sh`; modify `tests/host/Makefile`

**Acceptance Criteria:**
- [ ] Signature takes `int count` and loops it, never `LFO_COUNT`
- [ ] With `LFO_COUNT` force-defined to 8, a `count = 2` call leaves a guard element
      after a 2-element array untouched
- [ ] Retrigger and held-note counting behave as before
- [ ] `count <= 0` and a NULL array are no-ops, not wild reads

**Verify:** `bash tests/host/test_lfo_process_midi_count.sh` → `ALL PASS`

**Steps:**

- [ ] **Step 1: Write the failing test.** Create `tests/host/test_lfo_process_midi_count.c`:

```c
/*
 * lfo_process_midi must loop the COUNT IT IS GIVEN, not the global LFO_COUNT.
 *
 * Master FX keeps 2 routes while a slot has 8, and both share this header. A
 * loop bounded by the macro reads six elements past shadow_master_fx_lfos, on
 * the SPI callback. The guard element below is what that overrun would hit.
 * Compiled with -DLFO_COUNT=8 so the macro is DELIBERATELY WRONG for this
 * array: the test fails if the loop still reads it.
 */
#include <stdio.h>
#include <string.h>
#include "host/lfo_common.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else printf("  ok  %s\n", (msg)); \
} while (0)

int main(void) {
    struct { lfo_state_t routes[2]; lfo_state_t guard; } arena;
    memset(&arena, 0, sizeof(arena));
    for (int i = 0; i < 2; i++) arena.routes[i].retrigger = 1;
    arena.guard.retrigger = 1;
    arena.guard.phase = 0.75;

    const uint8_t note_on[3]  = { 0x90, 60, 100 };
    const uint8_t note_on2[3] = { 0x90, 64, 100 };
    const uint8_t note_off[3] = { 0x80, 60, 0 };

    lfo_process_midi(arena.routes, 2, note_on, 3);
    CHECK(arena.routes[0].held_count == 1, "route 0 counted the note on");
    CHECK(arena.routes[1].held_count == 1, "route 1 counted the note on");
    CHECK(arena.guard.held_count == 0, "the guard past the array was NOT touched");
    CHECK(arena.guard.phase == 0.75, "the guard's phase was NOT reset");

    arena.routes[0].phase = 0.5;
    lfo_process_midi(arena.routes, 2, note_on2, 3);
    CHECK(arena.routes[0].phase == 0.5, "a second held note does not retrigger");
    CHECK(arena.routes[0].held_count == 2, "held_count reached 2");

    lfo_process_midi(arena.routes, 2, note_off, 3);
    lfo_process_midi(arena.routes, 2, note_off, 3);
    CHECK(arena.routes[0].held_count == 0, "held_count returned to 0");

    arena.routes[0].phase = 0.5;
    lfo_process_midi(arena.routes, 2, note_on, 3);
    CHECK(arena.routes[0].phase == 0.0, "the first note of a NEW phrase retriggers");

    lfo_process_midi(arena.routes, 0, note_on, 3);
    CHECK(arena.routes[0].held_count == 1, "count=0 touched nothing");
    lfo_process_midi(NULL, 2, note_on, 3);
    CHECK(1, "a NULL array did not crash");

    printf(failures ? "FAIL\n" : "ALL PASS\n");
    return failures ? 1 : 0;
}
```

`tests/host/test_lfo_process_midi_count.sh`:

```bash
#!/usr/bin/env bash
# lfo_process_midi loops the count it is given, not the global LFO_COUNT.
set -euo pipefail
cd "$(dirname "$0")/../.."
make -s -C tests/host ../../build/tests/host/test_lfo_process_midi_count >/dev/null
exec ./build/tests/host/test_lfo_process_midi_count
```

`chmod +x` it. In `tests/host/Makefile`, copy the shape of the existing `test_bus_route`
rule, adding `-DLFO_COUNT=8`, and add the name to the `TESTS` list.

- [ ] **Step 2: Run it.** `bash tests/host/test_lfo_process_midi_count.sh`
      → compile error, `too many arguments to function 'lfo_process_midi'`. That is the red state.

- [ ] **Step 3: Change the signature** in `src/host/lfo_common.h`. `LFO_COUNT` stays
      defined (Master FX's sibling still uses its own count) but this function stops reading it:

```c
/* Process a MIDI message for retrigger: reset phase on first note-on of a phrase.
 *
 * COUNT IS A PARAMETER, not LFO_COUNT. This header is shared by the slot chain
 * (8 routes) and Master FX (MASTER_FX_LFO_COUNT, 2). A loop bounded by the
 * macro reads six elements past shadow_master_fx_lfos, on the SPI callback. The
 * two counts were equal when this was written, so the bug was invisible;
 * tests/host/test_lfo_process_midi_count.c compiles with LFO_COUNT forced to 8
 * so it cannot become invisible again. */
static inline void lfo_process_midi(lfo_state_t *lfos, int count,
                                    const uint8_t *msg, int len) {
    if (!lfos || count <= 0 || len < 3) return;
    uint8_t status = msg[0] & 0xF0;
    if (status == 0x90 && msg[2] > 0) {
        for (int i = 0; i < count; i++) {
            if (lfos[i].retrigger && lfos[i].held_count == 0) lfos[i].phase = 0.0;
            lfos[i].held_count++;
        }
    } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
        for (int i = 0; i < count; i++) {
            if (lfos[i].held_count > 0) lfos[i].held_count--;
        }
    }
}
```

- [ ] **Step 4: Update the one caller.** `chain_midi.c:758` →
      `lfo_process_midi(inst->lfos, LFO_COUNT, msg, len);`

- [ ] **Step 5: Verify.**
      `bash tests/host/test_lfo_process_midi_count.sh` → `ALL PASS`;
      `make -C tests/host test` → all `PASS`;
      `for t in tests/host/*.sh; do bash "$t" >/dev/null || echo "FAIL: $t"; done` → silent.

- [ ] **Step 6: Commit** — `lfo_common: loop the count you were given, not the global LFO_COUNT`

---

### Task 1: `mod_src.h` — the source types and their arithmetic

**Goal:** A pure, natively testable header turning a route's source type plus latched
MIDI into the same bipolar `signal` an LFO produces.

**Why a separate header:** `bus_route.h`, `send_fx_key.h` and `master_fx_key.h` set the
precedent, and their preambles say why — routing and scaling code that can only be built
on the device is exactly the code that ships untested.

**Files:** Create `src/host/mod_src.h`, `tests/host/test_mod_src.c`, `.sh`; modify `tests/host/Makefile`

**Acceptance Criteria:**
- [ ] `MOD_SRC_LFO == 0`, so a zeroed route is an LFO and **no patch needs migrating**
- [ ] `mod_src_signal()` returns bipolar −1..+1 for every source, matching
      `lfo_compute_shape`, so downstream depth/polarity maths is unchanged
- [ ] Velocity 0 → −1.0, 64 → ≈0.0, 127 → +1.0; note and CC use the same map
- [ ] Rest values are *not* zero — an unplayed velocity route must not sit hard left
- [ ] An out-of-range CC number is clamped, not a wild read
- [ ] `mod_src_slew()` converges and is **exactly** idempotent at the target
- [ ] `mod_src_name()` never returns NULL

**Verify:** `bash tests/host/test_mod_src.sh` → `ALL PASS`

**Steps:**

- [ ] **Step 1: Write the failing test.** `tests/host/test_mod_src.c`:

```c
/*
 * mod_src.h -- the source arithmetic, run natively. Everything here is pure.
 * The point of the header is that the mapping from a 7-bit MIDI byte to the
 * bipolar signal the mod bus expects is testable OFF the device, because the
 * alternative is discovering on hardware that velocity 64 is not centre.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "host/mod_src.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else printf("  ok  %s\n", (msg)); \
} while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 0.005f)

int main(void) {
    CHECK(MOD_SRC_LFO == 0, "MOD_SRC_LFO is 0: a zeroed route is an LFO");

    for (int t = 0; t < MOD_SRC_COUNT; t++) {
        const char *n = mod_src_name(t);
        CHECK(n && n[0], "every type has a name");
        CHECK(mod_src_from_name(n) == t, "the name round-trips to its type");
    }
    CHECK(strcmp(mod_src_name(-1), "lfo") == 0, "a negative type reads as lfo");
    CHECK(strcmp(mod_src_name(MOD_SRC_COUNT), "lfo") == 0, "out of range reads as lfo");
    CHECK(mod_src_from_name("nonsense") == MOD_SRC_LFO, "an unknown name is lfo");
    CHECK(mod_src_from_name(NULL) == MOD_SRC_LFO, "NULL is lfo");

    mod_input_t in;
    mod_input_reset(&in);
    CHECK(in.velocity == 64, "velocity rests at centre, not hard left");
    CHECK(NEAR(mod_src_signal(MOD_SRC_PRESSURE, &in, 0), -1.0f),
          "pressure rests at the bottom of its span");

    in.velocity = 0;
    CHECK(NEAR(mod_src_signal(MOD_SRC_VELOCITY, &in, 0), -1.0f), "velocity 0 is -1.0");
    in.velocity = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_VELOCITY, &in, 0), 1.0f), "velocity 127 is +1.0");
    in.velocity = 64;
    CHECK(fabsf(mod_src_signal(MOD_SRC_VELOCITY, &in, 0)) < 0.02f, "velocity 64 is centre");

    in.note = 0;
    CHECK(NEAR(mod_src_signal(MOD_SRC_NOTE, &in, 0), -1.0f), "note 0 is -1.0");
    in.note = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_NOTE, &in, 0), 1.0f), "note 127 is +1.0");

    mod_input_reset(&in);
    in.cc[74] = 127;
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 74), 1.0f), "the route reads its own CC");
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 1), -1.0f), "another CC reads its own value");
    CHECK(NEAR(mod_src_signal(MOD_SRC_CC, &in, 200), -1.0f), "an out-of-range CC is clamped");

    CHECK(mod_src_signal(MOD_SRC_COUNT, &in, 0) == 0.0f, "an unknown type is silent");
    CHECK(mod_src_signal(MOD_SRC_VELOCITY, NULL, 0) == 0.0f, "a NULL input is silent");
    CHECK(mod_src_signal(MOD_SRC_LFO, &in, 0) == 0.0f,
          "LFO is 0 here -- its signal comes from lfo_compute_shape, which owns phase");

    float v = -1.0f;
    for (int i = 0; i < 2000; i++) v = mod_src_slew(v, 1.0f, 0.05f);
    CHECK(NEAR(v, 1.0f), "slew converges on its target");
    CHECK(mod_src_slew(1.0f, 1.0f, 0.05f) == 1.0f, "slew at the target is exactly idempotent");
    CHECK(mod_src_slew(-1.0f, 1.0f, 0.0f) == 1.0f, "coeff 0 means no slew, i.e. jump");
    CHECK(mod_src_slew(0.0f, 1.0f, 1.0f) == 0.0f, "coeff 1 means never arrive");

    CHECK(mod_src_is_lfo(MOD_SRC_LFO) == 1, "LFO is flagged as the LFO type");
    CHECK(mod_src_is_lfo(MOD_SRC_VELOCITY) == 0, "velocity is not the LFO type");

    printf(failures ? "FAIL\n" : "ALL PASS\n");
    return failures ? 1 : 0;
}
```

`tests/host/test_mod_src.sh` mirrors Task 0's runner. Makefile rule:
`$(CC) $(CFLAGS) -I$(SRC) -o $@ $< -lm`, plus the name in `TESTS`.

- [ ] **Step 2: Run it.** → `fatal error: host/mod_src.h: No such file or directory`

- [ ] **Step 3: Write the header.** `src/host/mod_src.h`:

```c
/*
 * mod_src.h -- what a mod route's SOURCE is, and the arithmetic that turns it
 * into the signal the modulation bus already knows how to spend.
 *
 * Header-only and dependency-free, like bus_route.h and send_fx_key.h, so
 * tests/host can compile and RUN it natively. Their preambles carry the reason:
 * scaling code that can only be built on the device is exactly the code that
 * ships untested.
 *
 * THE OUTPUT CONTRACT IS lfo_compute_shape's: bipolar -1..+1. That is what makes
 * this a drop-in producer. chain_mod_emit_value already folds a unipolar route
 * by (signal + 1) * 0.5 and scales by the target's declared range, so a source
 * returning 0..1 here would be half-scaled twice -- quietly wrong rather than
 * obviously wrong.
 *
 * MOD_SRC_LFO IS 0 ON PURPOSE. Every route in every patch on disk predates this
 * field, so it parses as absent and memsets to zero -- and zero must mean "the
 * LFO it has always been". There is no migration step anywhere in this feature
 * because of that one line.
 *
 * Pure: no allocation, no I/O, no locks. Called from render_block and from the
 * param handler, i.e. from the SPI callback.
 */
#ifndef MOD_SRC_H
#define MOD_SRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MOD_SRC_LFO       0
#define MOD_SRC_VELOCITY  1
#define MOD_SRC_PRESSURE  2
#define MOD_SRC_CC        3
#define MOD_SRC_NOTE      4
#define MOD_SRC_COUNT     5

/* The WIRE names. Stored in the patch file and spoken by the param keys, so they
 * are part of the format: renaming one silently resets that route to LFO on the
 * next load (mod_src_from_name falls back rather than failing). The UI's display
 * words live in shadow_ui_slot_grid.mjs and may differ. */
static const char *const mod_src_names[MOD_SRC_COUNT] = {
    "lfo", "velocity", "pressure", "cc", "note"
};

/* An out-of-range type reads as "lfo" rather than NULL: every caller is building
 * a string to answer a param read, and a NULL there is a crash on the SPI
 * callback in exchange for nothing. */
static inline const char *mod_src_name(int type) {
    if (type < 0 || type >= MOD_SRC_COUNT) return mod_src_names[MOD_SRC_LFO];
    return mod_src_names[type];
}

static inline int mod_src_from_name(const char *name) {
    if (!name) return MOD_SRC_LFO;
    for (int i = 0; i < MOD_SRC_COUNT; i++)
        if (strcmp(name, mod_src_names[i]) == 0) return i;
    return MOD_SRC_LFO;
}

static inline int mod_src_is_lfo(int type) { return type == MOD_SRC_LFO; }

/*
 * What the slot last received, latched. One of these per chain instance.
 *
 * NOT per voice. The modulation bus moves a PARAMETER, through set_param, and a
 * parameter belongs to the plugin rather than to a note -- so velocity here is
 * last-note velocity and pressure is channel pressure, exactly as an outboard
 * mod matrix behaves. Poly velocity has to live inside the synth; it cannot be
 * expressed across the plugin boundary, and pretending otherwise here would be a
 * feature that sounds broken rather than one that is absent.
 *
 * REST VALUES ARE NOT ZERO, which is why mod_input_reset exists rather than a
 * memset. A velocity route aimed at cutoff, on a slot nobody has played yet,
 * must not sit at the bottom of its range -- the user hears a dead synth and
 * blames the target. Velocity and note rest at centre; pressure and CC, which a
 * player genuinely starts at zero, rest at the bottom.
 */
typedef struct {
    uint8_t velocity;   /* last note-on velocity, 0..127 */
    uint8_t pressure;   /* channel aftertouch, 0..127 */
    uint8_t note;       /* last note-on number, 0..127 */
    uint8_t cc[128];    /* last value seen per controller */
} mod_input_t;

#define MOD_SRC_VELOCITY_REST 64
#define MOD_SRC_PRESSURE_REST 0
#define MOD_SRC_NOTE_REST     64
#define MOD_SRC_CC_REST       0

static inline void mod_input_reset(mod_input_t *in) {
    if (!in) return;
    memset(in, 0, sizeof(*in));
    in->velocity = MOD_SRC_VELOCITY_REST;
    in->pressure = MOD_SRC_PRESSURE_REST;
    in->note     = MOD_SRC_NOTE_REST;
    for (int i = 0; i < 128; i++) in->cc[i] = MOD_SRC_CC_REST;
}

/* 7-bit byte -> bipolar. 63.5 rather than 63 or 64 so 0 and 127 both land
 * exactly on the rails; the midpoint then falls between 63 and 64, which is what
 * a symmetric 128-step span actually is. */
static inline float mod_src_7bit_bipolar(uint8_t v) {
    return ((float)v / 63.5f) - 1.0f;
}

/*
 * The route's signal, bipolar -1..+1.
 *
 * cc_num is read only for MOD_SRC_CC and is CLAMPED rather than trusted: it
 * arrives from a patch file and from a knob, and this runs on the SPI callback.
 *
 * MOD_SRC_LFO returns 0 here BY DESIGN -- an LFO's signal comes from
 * lfo_compute_shape, which owns phase, and duplicating that call inside this
 * header would give a route two phase accumulators. The caller branches on
 * mod_src_is_lfo(); see chain_host.c's mod_tick.
 */
static inline float mod_src_signal(int type, const mod_input_t *in, int cc_num) {
    if (!in) return 0.0f;
    switch (type) {
    case MOD_SRC_VELOCITY: return mod_src_7bit_bipolar(in->velocity);
    case MOD_SRC_PRESSURE: return mod_src_7bit_bipolar(in->pressure);
    case MOD_SRC_NOTE:     return mod_src_7bit_bipolar(in->note);
    case MOD_SRC_CC:
        if (cc_num < 0) cc_num = 0;
        if (cc_num > 127) cc_num = 127;
        return mod_src_7bit_bipolar(in->cc[cc_num]);
    default:               return 0.0f;   /* LFO, and anything unknown */
    }
}

/*
 * One-pole slew toward a target, per block.
 *
 * coeff is the fraction of the remaining distance KEPT each block: 0 jumps (no
 * slew) and 1 never arrives. Expressed that way round because the UI offers a
 * "Slew" amount where more means slower; inverting it here keeps the one place
 * that knows the direction next to the one place that documents it.
 *
 * The equality short-circuit is NOT an optimisation. Without it a 7-bit source
 * that has settled keeps producing values a hair from the target forever, and
 * chain_mod_apply_effective_value's MOD_FLOAT_CHANGE_EPSILON is then the only
 * thing standing between that and a set_param string write on every block.
 */
static inline float mod_src_slew(float current, float target, float coeff) {
    if (current == target) return current;
    if (coeff <= 0.0f) return target;
    if (coeff >= 1.0f) return current;
    return target + (current - target) * coeff;
}

#endif /* MOD_SRC_H */
```

- [ ] **Step 4: Run it.** → `ALL PASS`

- [ ] **Step 5: Prove the test can fail.** Commit first (`git checkout --` reverts to
      HEAD and would eat an uncommitted fix). Then mutate
      `in->velocity = MOD_SRC_VELOCITY_REST;` → `= 0;`, `rm -f build/tests/host/test_mod_src`
      (ExtFS has 1s mtime granularity), re-run → expect
      `FAIL: velocity rests at centre, not hard left`. Restore, rm the binary, re-run → `ALL PASS`.

---

### Task 2: `mod_route_t`, eight of them, and the `modN:` key ladder

**Goal:** Rename the concept in C, raise the count to 8, add the new fields, accept both
`modN:` and legacy `lfoN:`.

**Files:** `src/host/lfo_common.h`, `chain_internal.h:316,804-806`,
`chain_host.c:1119-1210` (set) and `:1741` (get); create `tests/host/test_mod_route_keys.sh`

**Acceptance Criteria:**
- [ ] `MOD_ROUTE_COUNT` is 8, defined **once**, in `chain_internal.h`
- [ ] `LFO_COUNT` stays 2 and is read only by Master FX's sibling; `grep -n LFO_COUNT
      src/modules/chain/dsp/` returns nothing
- [ ] `mod1:`..`mod8:` set and get every field
- [ ] `lfo1:`/`lfo2:` still address routes 1 and 2 — **the same storage, not a copy**
- [ ] `mod9:`, `mod0:`, `mod01:` and `lfo3:` are rejected, leaving state untouched
      (`master_fx_key.h`'s rule: an unmatched key never defaults to route 0)
- [ ] `mod1:src` accepts the wire name *and* a raw enum index, and returns the wire name
- [ ] The emit `source_id` is `mod%d` for all eight — **not** `lfo%d` for the first two,
      or a patch load clears a source that no longer exists and leaves the new one running

**Verify:** `bash tests/host/test_mod_route_keys.sh` → `ALL PASS`; full `tests/host` green;
`./scripts/build.sh` succeeds

**Steps:**

- [ ] **Step 1: Write the failing source pin.** `tests/host/test_mod_route_keys.sh`:

```bash
#!/usr/bin/env bash
#
# The modN: / legacy lfoN: key ladder.
#
# Pinned rather than trusted because this is the exact shape master_fx_key.h
# exists to fix: a hand-written strncmp ladder that restates its own cap. The
# properties below are the ones such a ladder loses first -- an out-of-range
# index landing on route 0, and a legacy alias that COPIES rather than aliases.
set -euo pipefail
cd "$(dirname "$0")/../.."
HOST="src/modules/chain/dsp/chain_host.c"
INT="src/modules/chain/dsp/chain_internal.h"
[ -f "$HOST" ] && [ -f "$INT" ] || { echo "FAIL: missing sources"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

n=$(/usr/bin/grep -c '^#define MOD_ROUTE_COUNT' "$INT" || true)
[ "$n" = "1" ] && say_ok "MOD_ROUTE_COUNT defined exactly once" \
               || say_fail "MOD_ROUTE_COUNT defined $n times, expected 1"
/usr/bin/grep -q '^#define MOD_ROUTE_COUNT 8' "$INT" \
  && say_ok "MOD_ROUTE_COUNT is 8" || say_fail "MOD_ROUTE_COUNT is not 8"

# The chain must have stopped reading the Master FX-shaped count entirely.
if /usr/bin/grep -rq 'LFO_COUNT' src/modules/chain/dsp/; then
  say_fail "src/modules/chain/dsp still reads LFO_COUNT; it must use MOD_ROUTE_COUNT"
else
  say_ok "the chain no longer reads LFO_COUNT"
fi

# Master FX keeps its own count. If it ever reads MOD_ROUTE_COUNT the two have
# been fused and Master FX silently grew six routes it has no UI for.
/usr/bin/grep -q 'MASTER_FX_LFO_COUNT' src/host/shadow_chain_mgmt.h \
  && say_ok "Master FX still names its own count" || say_fail "MASTER_FX_LFO_COUNT is gone"
if /usr/bin/grep -q 'MOD_ROUTE_COUNT' src/host/shadow_chain_mgmt.c; then
  say_fail "shadow_chain_mgmt.c reads MOD_ROUTE_COUNT; Master FX must stay at 2"
else
  say_ok "Master FX does not read the slot route count"
fi

# The modN: route must be PARSED, not enumerated. ("lfo1:"/"lfo2:" may be
# literal -- the legacy name is frozen at two by the format.)
if /usr/bin/grep -qE 'strncmp\(key, "mod[0-9]:"' "$HOST"; then
  say_fail "the modN: ladder enumerates indices; parse them instead"
else
  say_ok "the modN: ladder parses its index rather than enumerating"
fi

/usr/bin/grep -q 'chain_mod_route_index' "$HOST" \
  && say_ok "index resolution goes through one named helper" \
  || say_fail "no chain_mod_route_index helper; the ladder is open-coded"

# The emit source_id must be modN for every route, including 1 and 2.
if /usr/bin/grep -q 'source_id, sizeof(source_id), "lfo%d"' "$HOST"; then
  say_fail "an emit source_id is still lfo%d; all eight routes must emit as modN"
else
  say_ok "every route emits under a modN source_id"
fi

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
```

- [ ] **Step 2: Run it.** → `FAIL: MOD_ROUTE_COUNT defined 0 times…`

- [ ] **Step 3: Extend the struct.** In `lfo_common.h`, `#include "mod_src.h"` and
      **append** to `lfo_state_t` (appended so the patch restore memcpy keeps working
      and a zeroed struct is an LFO):

```c
    int held_count;       /* Number of currently held notes (for retrigger) */

    /* ---- Mod route fields ------------------------------------------------
     * A route whose src is not MOD_SRC_LFO ignores every field above except
     * target/param/depth/bipolar/enabled.
     *
     * APPENDED, and zero must stay meaningful: src == MOD_SRC_LFO == 0 is what
     * lets every patch written before this field parse as the LFO it was, with
     * no migration anywhere in the feature. */
    int src;              /* MOD_SRC_* */
    int cc_num;           /* MOD_SRC_CC only: which controller, 0..127 */
    float slew;           /* 0..0.99, the fraction of distance KEPT per block */
    float slewed;         /* runtime only, not persisted */
    int slew_primed;      /* runtime only: has slewed been seeded? */
```

In `chain_internal.h`, near `MAX_AUDIO_FX`:

```c
/* Mod routes a slot carries. Each is a source (LFO, velocity, pressure, CC or
 * note) aimed at one parameter of one component, summed by the modulation bus in
 * chain_mod.c.
 *
 * 8, matching MAX_AUDIO_FX / SLOT_BUSES / MASTER_FX_SLOTS -- this codebase's
 * number for "a chain of things". It costs sizeof(lfo_state_t) each in
 * chain_instance_t (heap) and in patch_info_t, which IS a stack local on
 * v2_set_param's load_file route; 2 -> 8 adds ~780 bytes to a 232 KB frame.
 *
 * MASTER_FX_LFO_COUNT is a DIFFERENT number and must stay 2: Master FX shares
 * lfo_common.h but has neither the sources nor the UI (and no MIDI input to read
 * a source from). lfo_process_midi takes an explicit count precisely so the two
 * cannot be confused. */
#define MOD_ROUTE_COUNT 8
```

Replace the arrays — `patch_info_t` (`:316`) gets `lfo_state_t mod_routes[MOD_ROUTE_COUNT];`,
and `chain_instance_t` (`:804-806`) gets `mod_routes[]`, `mod_route_base_values[]`,
`mod_route_base_valid[]` plus `mod_input_t mod_input;`.

- [ ] **Step 4: Rename mechanically.** Sites are exactly `chain_host.c`, `chain_midi.c:750,758`,
      `chain_patch.c:1479,1735`, `chain_reorder.c:113`. Rename `inst->lfos` → `inst->mod_routes`,
      `inst->lfo_base_values` → `inst->mod_route_base_values`, `inst->lfo_base_valid` →
      `inst->mod_route_base_valid`, `patch->lfos` → `patch->mod_routes`, and every remaining
      `LFO_COUNT` in those four files → `MOD_ROUTE_COUNT`. The test's grep pins the result.

- [ ] **Step 5: One index helper, then the ladder.** Add above `v2_set_param`:

```c
/*
 * Resolve a "mod<N>:" or legacy "lfo<N>:" prefix to a 0-based route index.
 *
 * Returns the index and points *out_rest at the byte after the colon; returns -1
 * on no match, leaving *out_rest untouched. It NEVER returns 0 for an unmatched
 * key -- master_fx_key.h's preamble is the war story: an else-branch that
 * assigned slot 0 turned an out-of-range key into a write to a DIFFERENT running
 * module under a garbage param name.
 *
 * The legacy spelling resolves to the SAME storage, not a copy. Routes 1 and 2
 * simply have two names; a patch written before this feature addresses them by
 * the old one and a patch written after by the new one, and both must be the
 * same two routes or a set loaded twice has four.
 */
static int chain_mod_route_index(const char *key, const char **out_rest) {
    if (!key) return -1;
    int max = MOD_ROUTE_COUNT;
    const char *p;
    if (strncmp(key, "mod", 3) == 0) {
        p = key + 3;
    } else if (strncmp(key, "lfo", 3) == 0) {
        p = key + 3;
        max = 2;   /* the legacy name only ever addressed two */
    } else {
        return -1;
    }
    if (*p < '1' || *p > '9') return -1;   /* rejects "mod0" and "mod01" */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (n < 1 || n > max) return -1;
    if (*p != ':') return -1;
    if (out_rest) *out_rest = p + 1;
    return n - 1;
}
```

Replace the `chain_host.c:1119-1120` condition with a single call (capture the index
once — do not call it twice), keep the existing per-subkey body, change `source_id` to
`"mod%d"`, and add three branches:

```c
        } else if (strcmp(subkey, "src") == 0) {
            /* Accepts the wire name AND a raw int: the UI writes an enum INDEX
             * (the knob grid's enum type is an index) while the patch file writes
             * the name. One branch rather than two call sites. */
            int t = (val[0] >= '0' && val[0] <= '9') ? atoi(val) : mod_src_from_name(val);
            if (t < 0 || t >= MOD_SRC_COUNT) t = MOD_SRC_LFO;
            if (t != lfo->src) {
                lfo->src = t;
                lfo->slew_primed = 0;   /* seed from the NEW source's value, not the old one's */
            }
            lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
        } else if (strcmp(subkey, "cc_num") == 0) {
            lfo->cc_num = atoi(val);
            if (lfo->cc_num < 0) lfo->cc_num = 0;
            if (lfo->cc_num > 127) lfo->cc_num = 127;
        } else if (strcmp(subkey, "slew") == 0) {
            lfo->slew = strtof(val, NULL);
            if (lfo->slew < 0.0f) lfo->slew = 0.0f;
            if (lfo->slew > 0.99f) lfo->slew = 0.99f;
        }
```

Mirror it in the getter at `:1741`: `mod_src_name(lfo->src)` for `src`, `%d` for
`cc_num`, `%.6f` for `slew`.

- [ ] **Step 6: Verify + report growth.** Full `tests/host` green; `./scripts/build.sh`
      succeeds. Report `sizeof(chain_instance_t)` before and after in the commit message.

---

### Task 3: Latch MIDI at both synth-feed paths

**Goal:** Populate `mod_input` from the MIDI the **synth** receives, so an arpeggiator's
velocity drives a velocity route.

**Why both paths:** `chain_record_synth_note`'s comment (`chain_midi.c:116-130`) is the
precedent and the war story. `v2_on_midi` carries notes a MIDI FX transformed in
`process_midi`; `v2_tick_midi_fx` carries notes it emitted from `tick()` — which is what
an arpeggiator does, swallowing the held note and emitting its pattern on the clock.
Instrumenting only the first means the latch never updates with an arp in the slot, and
the failure is silent: the route is enabled, aimed, and stuck at its rest value.

**Files:** `chain_midi.c:116-131` (new sibling fn), `:603`, `:932`; `chain_host.c`
(`create_instance`); create `tests/host/test_mod_input_latch.sh`

**Acceptance Criteria:**
- [ ] `chain_record_mod_input` is called at **both** `:603` and `:932`, adjacent to the
      existing `chain_record_synth_note` calls
- [ ] Note-on latches velocity and note; note-off and velocity-0 note-on latch neither
- [ ] Channel aftertouch (0xD0, **one data byte**) and poly aftertouch (0xA0, value byte)
      both latch pressure
- [ ] CC (0xB0) latches into `cc[]` by controller number, masked to 0..127
- [ ] The function does nothing but store — no logging, allocation, or file access
- [ ] `mod_input_reset` is called in `create_instance`

**Verify:** `bash tests/host/test_mod_input_latch.sh` → `ALL PASS`

**Steps:**

- [ ] **Step 1: Write the failing source pin.** `tests/host/test_mod_input_latch.sh`:

```bash
#!/usr/bin/env bash
#
# THE MOD INPUT IS LATCHED AT BOTH SYNTH-FEED PATHS.
#
# Same rule, same reason, as synth:last_note: v2_on_midi carries what a MIDI FX
# transformed in process_midi, v2_tick_midi_fx carries what one EMITTED from
# tick() -- which is what an arpeggiator does. Instrumenting only the first means
# a velocity route never moves with an arp in the slot, and the failure is
# silent: the route is enabled, aimed, and stuck at its rest value.
set -euo pipefail
cd "$(dirname "$0")/../.."
F="src/modules/chain/dsp/chain_midi.c"
[ -f "$F" ] || { echo "FAIL: missing $F"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

calls=$(/usr/bin/grep -c 'chain_record_mod_input(' "$F" || true)
[ "$calls" = "3" ] && say_ok "one definition and exactly two call sites" \
  || say_fail "found $calls occurrences of chain_record_mod_input, expected 3"

# Each call must sit beside its last_note sibling -- that adjacency IS the
# invariant. If a third synth-feed path appears, both must move together.
paired=$(/usr/bin/grep -A1 'chain_record_synth_note(inst, out_msgs\[i\], out_lens\[i\]);' "$F" \
         | /usr/bin/grep -c 'chain_record_mod_input' || true)
[ "$paired" = "2" ] && say_ok "both last_note sites are paired with a mod_input latch" \
  || say_fail "$paired of 2 last_note sites are paired; they must move together"

body=$(/usr/bin/sed -n '/^static inline void chain_record_mod_input/,/^}/p' "$F")
[ -n "$body" ] || say_fail "could not lift chain_record_mod_input()"
for bad in 'unified_log' 'fprintf' 'fopen' 'malloc' 'access(' 'snprintf' 'host->log'; do
  printf '%s' "$body" | /usr/bin/grep -q "$bad" \
    && say_fail "chain_record_mod_input calls $bad -- it runs on the SPI callback"
done
say_ok "the latch does no I/O, logging or allocation"

# Channel aftertouch has ONE data byte. Reading msg[2] for 0xD0 is the classic
# error and would latch whatever is stale in the buffer.
printf '%s' "$body" | /usr/bin/grep -A2 'case 0xD0' | /usr/bin/grep -q 'msg\[1\]' \
  && say_ok "channel aftertouch reads msg[1], its only data byte" \
  || say_fail "channel aftertouch does not read msg[1]"

/usr/bin/grep -q 'mod_input_reset(&inst->mod_input)' src/modules/chain/dsp/chain_host.c \
  && say_ok "mod_input is reset at instance creation" \
  || say_fail "mod_input is never reset; an unplayed slot rests at zero"

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
```

- [ ] **Step 2: Run it.** → `FAIL: found 0 occurrences…`

- [ ] **Step 3: Add the latch**, directly below `chain_record_synth_note`:

```c
/* Latch what the SYNTH receives, for the mod routes' MIDI sources.
 *
 * Deliberately a sibling of chain_record_synth_note and called at the same two
 * sites, for the same reason: an ARPEGGIATOR emits from tick(), not from
 * process_midi, so a latch on only v2_on_midi never updates with an arp in the
 * slot. A velocity route would sit at its rest value while the arp played, and
 * nothing would report it.
 *
 * POST-MIDI-FX on purpose. This is what the synth HEARD, not what the player
 * did -- a velocity curve or a chord FX in the slot is part of the instrument,
 * and a route that ignored it would disagree with the sound.
 *
 * Note-offs latch nothing: a released pad's velocity is RELEASE velocity, a
 * different control, and zeroing on release would make every velocity route snap
 * back between notes.
 *
 * Runs on the SPI callback -- stores and nothing else. */
static inline void chain_record_mod_input(chain_instance_t *inst,
                                          const uint8_t *msg, int len) {
    if (len < 2) return;
    switch (msg[0] & 0xF0) {
    case 0x90:  /* note on; velocity 0 is a note off */
        if (len >= 3 && msg[2] > 0) {
            inst->mod_input.velocity = msg[2];
            inst->mod_input.note = msg[1];
        }
        break;
    case 0xA0:  /* poly aftertouch -- collapsed to channel, see mod_input_t */
        if (len >= 3) inst->mod_input.pressure = msg[2];
        break;
    case 0xD0:  /* channel aftertouch -- ONE data byte */
        inst->mod_input.pressure = msg[1];
        break;
    case 0xB0:  /* control change */
        if (len >= 3) inst->mod_input.cc[msg[1] & 0x7F] = msg[2];
        break;
    default:
        break;
    }
}
```

At `:603` and `:932`, add `chain_record_mod_input(inst, out_msgs[i], out_lens[i]);`
immediately after each existing `chain_record_synth_note` call. In `create_instance`,
add `mod_input_reset(&inst->mod_input);`.

- [ ] **Step 4: Verify.** Test `ALL PASS`; full suite silent; `./scripts/build.sh` succeeds.

---

### Task 4: `mod_tick` — dispatch on the source

**Goal:** The per-block producer branches on `src`: LFO keeps its phase accumulator,
every other source reads the latch, and both land in the same `chain_mod_emit_value`.

**Files:** `chain_host.c:2225-2340` (`lfo_tick` → `mod_tick`), `:735` (forward decl),
`:769` (the `mod:tick` route); create `tests/host/test_mod_tick_dispatch.sh`

**Acceptance Criteria:**
- [ ] `src == MOD_SRC_LFO` behaves exactly as before — phase, sync, retrigger, S&H,
      mod-to-mod, and the send-amount offset path
- [ ] A non-LFO route does **not** advance phase
- [ ] Every source passes through `mod_src_slew` before `chain_mod_emit_value`
- [ ] The slew is **seeded** from the current source value on the first block and after
      a `src` change, never ramped from the previous source's value
- [ ] Mod-to-mod accepts `mod1`..`mod8` and legacy `lfo1`/`lfo2`, reusing
      `chain_mod_route_index` rather than a second parser
- [ ] The `target == "buses"` send path still bypasses the mod bus into `main_send_mod[]`

**Verify:** `bash tests/host/test_mod_tick_dispatch.sh` → `ALL PASS`;
`bash tests/host/test_lfo_send_target.sh` → still passes unchanged

**Steps:**

- [ ] **Step 1: Write the failing source pin.** `tests/host/test_mod_tick_dispatch.sh`:

```bash
#!/usr/bin/env bash
#
# mod_tick dispatches on the SOURCE, and the LFO path is unchanged.
#
# The trap this pins: a non-LFO route must not advance phase. It costs a sinf per
# block per route for a number nothing reads -- and worse, a route later switched
# back to LFO has had its phase running the whole time, so the waveform resumes
# from somewhere arbitrary instead of where the user left it.
set -euo pipefail
cd "$(dirname "$0")/../.."
F="src/modules/chain/dsp/chain_host.c"
body=$(/usr/bin/sed -n '/^static void mod_tick(/,/^}$/p' "$F")
[ -n "$body" ] || { echo "FAIL: could not lift mod_tick()"; exit 1; }

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

for sym in mod_src_is_lfo mod_src_signal mod_src_slew; do
  printf '%s' "$body" | /usr/bin/grep -q "$sym" \
    && say_ok "mod_tick calls $sym" || say_fail "mod_tick never calls $sym"
done

# Phase advance must sit INSIDE the LFO branch.
if printf '%s' "$body" \
   | /usr/bin/awk '/mod_src_is_lfo/{in_lfo=1} /^        } else \{/{in_lfo=0} /lfo_advance_phase/{if(!in_lfo) print "outside"}' \
   | /usr/bin/grep -q outside; then
  say_fail "lfo_advance_phase runs outside the LFO branch"
else
  say_ok "phase only advances for LFO routes"
fi

# Mod-to-mod must not restate a two-wide literal range.
if printf '%s' "$body" | /usr/bin/grep -qE "target\[3\] >= '1' && .*target\[3\] <= '2'"; then
  say_fail "mod-to-mod still hardcodes a 1..2 range"
else
  say_ok "mod-to-mod is not pinned to two routes"
fi
printf '%s' "$body" | /usr/bin/grep -q 'chain_mod_route_index' \
  && say_ok "mod-to-mod reuses the one index parser" \
  || say_fail "mod-to-mod parses its own index; reuse chain_mod_route_index"

printf '%s' "$body" | /usr/bin/grep -q 'main_send_mod' \
  && say_ok "the send-amount offset path survives" \
  || say_fail "main_send_mod is gone; sends would be written destructively"

# The slew must be SEEDED, not ramped, after a src change.
printf '%s' "$body" | /usr/bin/grep -q 'slew_primed' \
  && say_ok "the slew is seeded via slew_primed" \
  || say_fail "no slew_primed seeding; a src change glides from the old source"

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
```

- [ ] **Step 2: Run it.** → `FAIL: could not lift mod_tick()` (still named `lfo_tick`).

- [ ] **Step 3: Rename and restructure.** Rename `lfo_tick` → `mod_tick` at its
      definition, forward declaration (`:735`) and call site (`:769`). The `mod:tick`
      **param key** does not change — it was already named for the concept.

      Inside the loop, replace only the phase-and-shape block. Everything after `signal`
      is computed — the send path, mod-to-mod, `chain_mod_emit_value` — stays as-is:

```c
    for (int i = 0; i < MOD_ROUTE_COUNT; i++) {
        lfo_state_t *lfo = &inst->mod_routes[i];
        if (!lfo->active) continue;

        float signal;
        if (mod_src_is_lfo(lfo->src)) {
            /* ---- unchanged: the existing phase / sync / shape block ------- */
            /* (beat-position branch, lfo_sync_rate_hz, lfo_advance_phase,
             *  phase_offset, lfo_compute_shape) */
            signal = lfo_compute_shape(lfo->shape, effective_phase, lfo);
        } else {
            /*
             * A MIDI source. Phase is deliberately NOT advanced: it costs a sinf
             * per block for a number nothing reads, and it would leave a route
             * switched back to LFO resuming from an arbitrary point rather than
             * where the user left it.
             */
            signal = mod_src_signal(lfo->src, &inst->mod_input, lfo->cc_num);
        }

        /*
         * Slew, for EVERY source including the LFO -- an LFO with slew 0 is
         * unchanged (mod_src_slew returns the target), so this is one code path
         * rather than a branch that has to be kept in step with the one above.
         *
         * SEEDED, not ramped, on the first block and after a src change. A route
         * switched from Velocity to Pressure otherwise glides from the old
         * source's value to the new one's, which sounds like a bug in the synth
         * rather than a transition in the matrix.
         */
        if (!lfo->slew_primed) {
            lfo->slewed = signal;
            lfo->slew_primed = 1;
        } else {
            lfo->slewed = mod_src_slew(lfo->slewed, signal, lfo->slew);
        }
        signal = lfo->slewed;

        /* ---- everything below here is unchanged ------------------------- */
```

      Widen mod-to-mod by **reusing** the one parser (`chain_mod_route_index` requires a
      trailing colon, which a bare target lacks — hence the probe):

```c
        /* Mod-to-mod: "mod1".."mod8", and legacy "lfo1"/"lfo2". Reuses the one
         * index parser rather than adding a second that can drift from it. */
        int target_route = -1;
        {
            char probe[24];
            const char *rest = NULL;
            int n = snprintf(probe, sizeof(probe), "%s:", lfo->target);
            if (n > 0 && n < (int)sizeof(probe))
                target_route = chain_mod_route_index(probe, &rest);
        }
```

      Rename `target_lfo` → `target_route` through the rest of the block and change its
      bound to `MOD_ROUTE_COUNT`. Add the three new fields to `slot_lfo_param_meta[]`
      (`:2203`) so mod-to-mod can target them: `src` 0..`MOD_SRC_COUNT-1`, `cc_num`
      0..127, `slew` 0..0.99.

- [ ] **Step 4: Verify.** Both named tests `ALL PASS`; full suite silent;
      `./scripts/build.sh` succeeds.

---

### Task 5: Patch persistence — write `mod_routes`, keep reading `lfos`

**Goal:** Existing sets load with their LFOs intact; new sets round-trip all eight routes.

**Files:** `chain_patch.c:1476-1520` (parse), the serializer, `:1735` (restore);
`tests/host/test_chain_patch_roundtrip.c`

**Acceptance Criteria:**
- [ ] A patch with only legacy `"lfos"` loads into routes 0 and 1 with `src == MOD_SRC_LFO`
- [ ] A patch with `"mod_routes"` loads all eight
- [ ] A patch with **both** prefers `"mod_routes"` and does not double-apply
- [ ] The serializer writes `"mod_routes"` only; `"lfos"` is read-only
- [ ] `src` is written as its **wire name**; `slew` and `cc_num` round-trip exactly
- [ ] `slewed` and `slew_primed` are NOT persisted — a saved slew position would
      reproduce a transient on load
- [ ] An unknown `src` name loads as LFO rather than rejecting the patch

**Verify:** `bash tests/host/test_chain_patch_roundtrip.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Add the failing cases** to `test_chain_patch_roundtrip.c`, in the file's
      existing `CHECK` idiom. Assert **exact** equality on persisted floats, per
      `split_join_roundtrip_must_be_exact`:

```c
    /* ---- legacy: a patch written before mod routes existed ------------- */
    {
        const char *legacy =
            "{\"lfos\":{\"lfo1\":{\"enabled\":1,\"shape\":2,\"depth\":0.5,"
            "\"target\":\"fx1\",\"param\":\"mix\"},\"lfo2\":null}}";
        patch_info_t p; memset(&p, 0, sizeof(p));
        parse_patch_json(legacy, &p);
        CHECK(p.mod_routes[0].enabled == 1, "legacy lfo1 loaded");
        CHECK(p.mod_routes[0].shape == 2, "legacy shape survived");
        CHECK(p.mod_routes[0].src == MOD_SRC_LFO,
              "a legacy route is an LFO: absent src parses as 0");
        CHECK(strcmp(p.mod_routes[0].target, "fx1") == 0, "legacy target survived");
        CHECK(p.mod_routes[1].enabled == 0, "a null legacy route stays inactive");
    }
    /* ---- the new form, all eight, with the new fields ------------------ */
    {
        const char *modern =
            "{\"mod_routes\":{\"mod1\":{\"enabled\":1,\"src\":\"velocity\","
            "\"depth\":-0.25,\"target\":\"synth\",\"param\":\"cutoff\"},"
            "\"mod8\":{\"enabled\":1,\"src\":\"cc\",\"cc_num\":74,\"slew\":0.5,"
            "\"target\":\"fx2\",\"param\":\"mix\"}}}";
        patch_info_t p; memset(&p, 0, sizeof(p));
        parse_patch_json(modern, &p);
        CHECK(p.mod_routes[0].src == MOD_SRC_VELOCITY, "mod1 is a velocity route");
        CHECK(p.mod_routes[0].depth == -0.25f, "a negative depth round-trips exactly");
        CHECK(p.mod_routes[7].src == MOD_SRC_CC, "mod8 is a CC route");
        CHECK(p.mod_routes[7].cc_num == 74, "cc_num round-trips");
        CHECK(p.mod_routes[7].slew == 0.5f, "slew round-trips exactly");
        CHECK(p.mod_routes[7].slew_primed == 0, "slew_primed is not persisted");
    }
    /* ---- an unknown source name is an LFO, not a failure --------------- */
    {
        const char *odd = "{\"mod_routes\":{\"mod1\":{\"src\":\"telepathy\"}}}";
        patch_info_t p; memset(&p, 0, sizeof(p));
        parse_patch_json(odd, &p);
        CHECK(p.mod_routes[0].src == MOD_SRC_LFO,
              "an unknown src name falls back to LFO rather than rejecting the patch");
    }
    /* ---- both sections present: the new one wins, once ----------------- */
    {
        const char *both = "{\"lfos\":{\"lfo1\":{\"depth\":0.25}},"
                           "\"mod_routes\":{\"mod1\":{\"depth\":0.75}}}";
        patch_info_t p; memset(&p, 0, sizeof(p));
        parse_patch_json(both, &p);
        CHECK(p.mod_routes[0].depth == 0.75f, "mod_routes wins over the legacy section");
    }
```

- [ ] **Step 2: Run it.** → `FAIL: mod1 is a velocity route`

- [ ] **Step 3: Implement.** Replace the `"lfos"` block with a loop over two section
      descriptors, reusing the existing per-object parser verbatim — which is exactly
      why this is a loop rather than two copies:

```c
    /*
     * Mod routes. TWO section names, and the newer one wins.
     *
     * "lfos" is what every patch written before this feature carries, with keys
     * "lfo1" and "lfo2". It is READ-ONLY: nothing writes it any more, so a set
     * saved once migrates itself and a set never opened keeps loading. There is
     * no migration pass and no version stamp, because MOD_SRC_LFO is 0 -- an
     * absent "src" memsets to exactly the source those routes already had.
     *
     * Order matters: legacy first, new second, so a file holding both ends up
     * with the new one. The other way round would let a stale "lfos" section
     * overwrite a route the user edited.
     */
    static const struct { const char *section; const char *item; int count; }
        route_sections[] = {
            { "\"lfos\"",       "lfo", 2 },
            { "\"mod_routes\"", "mod", MOD_ROUTE_COUNT },
        };
    for (size_t s = 0; s < sizeof(route_sections)/sizeof(route_sections[0]); s++) {
        const char *sec = strstr(json, route_sections[s].section);
        if (!sec) continue;
        for (int i = 0; i < route_sections[s].count; i++) {
            char item_key[16];
            snprintf(item_key, sizeof(item_key), "\"%s%d\"",
                     route_sections[s].item, i + 1);
            /* ... existing per-object body, writing into patch->mod_routes[i] ... */
        }
    }
```

      Inside the per-object body add, beside the existing fields:

```c
            {
                char src_name[16];
                if (json_get_string(obj, "src", src_name, sizeof(src_name)) > 0)
                    patch->mod_routes[i].src = mod_src_from_name(src_name);
            }
            json_get_int(obj, "cc_num", &patch->mod_routes[i].cc_num);
            json_get_float(obj, "slew", &patch->mod_routes[i].slew);
```

      In the serializer write `"mod_routes"` with `mod%d` keys and `mod_src_name(src)`;
      delete the `"lfos"` writer; do not write `slewed`/`slew_primed`. At `:1735` the
      restore loop's `source_id` becomes `"mod%d"` and it must also clear
      `slew_primed = 0` so the slew seeds from the live source rather than the file.

- [ ] **Step 4: Verify.** Round-trip `PASS`; full suite silent; `./scripts/build.sh` succeeds.

---

### Task 6: Permutation remap covers all eight routes

**Goal:** An FX insert/remove/move re-aims every mod route, not just the first two.

**Files:** `chain_reorder.c:113`; `tests/host/test_chain_shape_verbs.sh`

**Acceptance Criteria:**
- [ ] The remap loop is bounded by `MOD_ROUTE_COUNT`
- [ ] A route aimed at a moved position follows its module
- [ ] A route aimed at a removed position is deactivated with its `param` cleared,
      matching the existing behaviour for routes 0 and 1

**Verify:** `bash tests/host/test_chain_shape_verbs.sh` → `ALL PASS`

**Steps:**

- [ ] **Step 1: Add the failing assertion** to `test_chain_shape_verbs.sh`:

```bash
# Every mod route is remapped, not just the two that used to exist. A route left
# aimed at a stale position is WORSE than a dropped one: fx3 still exists after a
# move, so the route keeps modulating -- the wrong module.
if /usr/bin/grep -qE 'for \(int i = 0; i < (LFO_COUNT|2); i\+\+\)' src/modules/chain/dsp/chain_reorder.c; then
  say_fail "chain_reorder still bounds a retarget loop by the old LFO count"
else
  say_ok "no retarget loop is bounded by the old LFO count"
fi
/usr/bin/grep -q 'i < MOD_ROUTE_COUNT' src/modules/chain/dsp/chain_reorder.c \
  && say_ok "the mod-route retarget loop covers all eight" \
  || say_fail "no MOD_ROUTE_COUNT-bounded retarget loop in chain_reorder.c"
```

- [ ] **Step 2: Run it.** → `FAIL: no MOD_ROUTE_COUNT-bounded retarget loop…`
      (Task 2's rename touched the array name; the bound is still wrong.)
- [ ] **Step 3: Fix the bound** at `chain_reorder.c:113`.
- [ ] **Step 4: Verify.** Test `ALL PASS`; full suite silent.

---

### Task 7: The knob grid — `modParams` and the Source cell

**Goal:** Eight route pages, each with a Source cell whose value decides which other
cells exist.

**The page-budget problem, and the decision.** A page is eight knobs. Today an LFO
declares nine params but only eight are ever *visible* — `rate_hz` and `rate_div` are
mutually exclusive on `sync`, which is the trick that makes it fit. Adding `src` makes
nine visible in the LFO case. One must go.

**`phase_offset` is the one.** Ranked by how often a user turns them, phase is last: it
only matters when two routes run at the same rate and you want them offset. It becomes a
declared param that is **not** a knob — the same treatment `rate_div`'s own comment
describes on this page — so it lands on the route's overflow page and stays editable. It
also **drops its `viz` role**: `lfoHeights(w, shape, rateFrac, depth, phase = 0)`
defaults phase to 0, so the waveform still draws, and a role declared for a cell that is
not on the page would leave the group with one member on page 2.

For every non-LFO source the page is far under budget: `src`, `target`, `enabled`,
`polarity`, `depth`, `slew`, plus `cc_num` for CC.

**Master FX gets no Source cell.** It processes the mixed bus and has no MIDI input, so
there is no `mod_input` for a velocity or pressure source to read. Offering the cell
there would be a control that silently does nothing — worse than its absence.

**Files:** `src/shadow/shadow_ui_slot_grid.mjs:125-330`;
`tests/host/test_shadow_slot_grid_contract.sh`; `tests/fixtures/chain-editor-baseline.txt`

**Acceptance Criteria:**
- [ ] `modParams(n, prefix)` exists; `lfoParams` is gone, **not** kept as a wrapper
- [ ] `src == lfo` → exactly 8 visible knobs
- [ ] `src == velocity|pressure|note` → src, target, enabled, polarity, depth, slew;
      shape/sync/rate/phase all hidden
- [ ] `src == cc` → the same plus `cc_num`
- [ ] `phase_offset` is a declared param but is NOT in `modKnobKeys`
- [ ] The `lfo` viz group still forms and still draws — **verified by rendering**, not
      by reading the code
- [ ] Master FX pages are pixel-identical to before this task
- [ ] Every `visible_if` condition key carries its own prefix
      (`normalizeVisibilityConditionKey` passes any key containing `:` straight through)

**Verify:** `bash tests/host/test_shadow_slot_grid_contract.sh` → `ALL PASS`, plus
rendered PNGs of each source state inspected by eye

**Steps:**

- [ ] **Step 1: Extend the contract test.** In `test_shadow_slot_grid_contract.sh`, in
      its existing import-and-evaluate style:

```javascript
import { modParams, modKnobKeys } from "../../src/shadow/shadow_ui_slot_grid.mjs";

/* Resolve visible_if the way the planner does. */
const holds = (c, values) =>
  c.all ? c.all.every((x) => holds(x, values))
        : String(values[c.param]) === String(c.equals);
const knobs = new Set(modKnobKeys(1));
const onPage = (values) =>
  modParams(1).filter((p) => (!p.visible_if || holds(p.visible_if, values))
                             && knobs.has(p.key));

/* src is an enum INDEX on the wire: 0 lfo, 1 vel, 2 prs, 3 cc, 4 note. */
check(onPage({ "mod1:src": 0, "mod1:sync": 0 }).length === 8,
      "an LFO route fills exactly the eight knobs");
check(onPage({ "mod1:src": 1 }).length === 6, "a velocity route shows six knobs");
check(onPage({ "mod1:src": 3 }).length === 7, "a CC route shows six plus cc_num");
check(!onPage({ "mod1:src": 1 }).some((p) => /shape|sync|rate|phase/.test(p.key)),
      "no LFO-only cell survives a non-LFO source");
check(!knobs.has("mod1:phase_offset"), "phase_offset is declared but is not a knob");
check(modParams(1).some((p) => p.key === "mod1:phase_offset"),
      "phase_offset is still editable on the overflow page");
check(!modParams(1).some((p) => p.key === "mod1:phase_offset" && p.viz),
      "phase_offset dropped its viz role with its knob");

const mfx = modParams(1, "master_fx:");
check(!mfx.some((p) => p.key === "master_fx:mod1:src"),
      "Master FX gets no Source cell -- it has no mod_input to read");
check(mfx.every((p) => !p.visible_if || JSON.stringify(p.visible_if).includes("master_fx:")),
      "every Master FX condition key carries the prefix");
```

- [ ] **Step 2: Run it.** → `modParams is not exported`

- [ ] **Step 3: Rewrite the builder.** Rename `lfoParams`/`lfoKnobKeys`/`lfoLevels` →
      `modParams`/`modKnobKeys`/`modLevels`. **Keep the existing doc comment** about the
      two contracts and the prefix trap — still true, still load-bearing — and extend it
      with the page-budget and Master-FX reasoning above. Add:

```javascript
export const MOD_SOURCES = ["LFO", "Velocity", "Pressure", "CC", "Note"];
export const MOD_SOURCES_SHORT = ["LFO", "VEL", "PRS", "CC", "NTE"];
```

      In the builder, derive `isSlot = keyPrefix === ""`. Gate the LFO-only params with
      a condition that is **compound on a slot and simple on Master FX** — on Master FX
      there is no `src` key at all, and a condition whose param reads empty compares
      false and would hide the whole page:

```javascript
    const lfoOnly = isSlot ? { visible_if: { param: k("src"), equals: "0" } } : {};
    const rateWhen = (syncVal) => isSlot
        ? { all: [{ param: k("sync"), equals: syncVal },
                  { param: k("src"),  equals: "0" }] }
        : { param: k("sync"), equals: syncVal };
```

      Emit `src` (slot only), then the existing `target`/`enabled`/`polarity`, then
      `sync`/`shape` with `...lfoOnly`, the two rate params with `visible_if:
      rateWhen("0"|"1")`, `depth`, then slot-only `cc_num` (`int` 0..127, default 74,
      gated `equals: "3"`) and `slew` (`float` 0..0.99), and finally `phase_offset` with
      `...lfoOnly` and **no `viz`**. `modKnobKeys` filters `phase_offset` out.

      **If the planner does not support `all:` in `visible_if`,** check
      `src/shared/param_pages/page_plan.mjs`'s evaluator first and add it there as part
      of this task. A single-condition evaluator cannot express "an LFO's rate cell";
      splitting into four mutually exclusive params instead is four cells to hide
      instead of two.

      Update `modLevels` to build `mod1`..`mod8` labelled `Mod 1`…, the call sites at
      `:313-318` to `modLevels([1..8])` for a slot and `modLevels([1,2], "master_fx:")`
      for Master FX, and the `/^lfo[12]:/` regexes at `:335` and `:404` to `/^mod[1-8]:/`.

- [ ] **Step 4: Render each page and LOOK at it.** Reading the code is not evidence —
      seven defects have sat in PNGs on this screen before. Render `mod1` with `src` =
      0..4 and one `master_fx:mod1` at 128×64 using the repo's bench
      (`src/shared/draw_bench.mjs` / `tools/param-pages/`) and open them. Confirm: the
      LFO waveform still spans its cells and is not clipped; no page has a gap where a
      hidden param was; `CC#` reads as a number, not an enum square; the Master FX page
      is unchanged.

- [ ] **Step 5: Verify.** Contract test `ALL PASS`; full suite silent.
      `test_chain_editor_snapshot.sh` and `tests/fixtures/chain-editor-baseline.txt` need
      regenerating — the slot's level list grew. Regenerate with the file's documented
      command and **read the diff**: six new levels is expected, anything else is a finding.

---

### Task 8: The picker, the labels, and the mod-to-mod range

**Goal:** The target picker offers eight routes, and every place that prints a route's
name says "Mod N" and names its source.

**Files:** `src/shadow/shadow_ui.js`, `src/shared/lfo_target_label.mjs`,
`src/shared/lfo_target_groups.mjs`; `tests/host/test_lfo_target_components.sh`,
`test_lfo_send_target.sh`; create `tests/host/test_mod_route_count_js.sh`

**Acceptance Criteria:**
- [ ] The picker offers `mod1`..`mod8` **excluding the route being edited** — a route
      must not target itself
- [ ] `SENDS_LFO_TARGET_KEY` (`"buses"`) still appears and still resolves
- [ ] A label reads `Mod 3 → fx2:cutoff`, and a non-LFO route names its source
      (`Vel → fx2:cutoff`)
- [ ] The grouping stays **LOSSLESS** — every key lands in a group, asserted over the
      fixture corpus, per `docs/SHADOW_UI.md`
- [ ] The cursor still lands on the routing the route already has
- [ ] The JS route count is pinned against `MOD_ROUTE_COUNT` by a new test in the shape
      of `test_master_fx_slots_js.sh`

**Verify:** all three named tests `ALL PASS`

**Steps:**

- [ ] **Step 1: Widen the existing tests.** In `test_lfo_target_components.sh`:

```javascript
const comps = lfoTargetComponentsFor(3);   /* editing route 3 */
check(comps.filter((c) => /^mod[1-8]$/.test(c.key)).length === 7,
      "seven other routes are offered; a route cannot target itself");
check(!comps.some((c) => c.key === "mod3"),
      "the route being edited is not in its own target list");
check(comps.some((c) => c.key === "buses"), "the sends target survives the widening");
```

- [ ] **Step 2: Run it.** → `FAIL: seven other routes are offered…` (two are).

- [ ] **Step 3: Widen the source.** Replace every literal `1..2` route range in
      `shadow_ui.js` with a loop over one constant:

```javascript
/* The slot's mod-route count. MUST equal MOD_ROUTE_COUNT in
 * src/modules/chain/dsp/chain_internal.h -- the C side sizes its arrays from
 * that name and this side draws the pages. A disagreement is a page that reads
 * and writes a route the DSP does not have, which answers "" and looks like a
 * dead knob rather than like a mismatch. Pinned by
 * tests/host/test_mod_route_count_js.sh, in the shape of
 * test_master_fx_slots_js.sh, which exists because a _Static_assert cannot span
 * the two languages. */
const MOD_ROUTE_COUNT = 8;
```

      Create `tests/host/test_mod_route_count_js.sh` as a near-copy of
      `test_master_fx_slots_js.sh`, reading the C `#define` and the JS constant and
      failing on drift.

      In `lfo_target_label.mjs`, add the source word — the label is what the Targ cell
      and the held-knob header show, so it must say what the route **is**:

```javascript
/* A route's label names its SOURCE, not just its destination.
 *
 * "-> fx2:cutoff" was unambiguous when every route was an LFO. With five source
 * types it is the more important half: two routes aimed at the same parameter,
 * one from Velocity and one from an LFO, are a completely different instrument,
 * and the cell has room for three characters of source. */
export const MOD_SOURCE_LABELS = ["LFO", "Vel", "Prs", "CC", "Note"];
```

- [ ] **Step 4: Verify.** All three tests `ALL PASS`; full suite silent.

---

### Task 9: Hardware verification

**Goal:** Prove the feature works on the device, because **nothing above does**.

**USER-ORDERED GATE — NON-SKIPPABLE.** Every task before this one is a source pin or a
native unit; not one has run the SPI callback.
`poc_module_found_the_ordering_defect` is the standing lesson: nine tasks and ~100 green
assertions once shipped a feature that did not function, because source pins cannot see
call ordering.

**Acceptance Criteria:**
- [ ] `./scripts/build.sh` succeeds; `./scripts/install.sh local --skip-modules --skip-confirmation` deploys
- [ ] A set saved **before** this branch loads with its LFOs still running — the legacy
      alias, end to end
- [ ] Mod 3, `src = Velocity`, aimed at a synth's cutoff: soft pads dark, hard pads
      bright — **and** the target's own knob still reads the BASE value (the
      `:base`/`:effective` split, checked from the UI)
- [ ] **The same route with an arpeggiator in the slot follows the ARP's velocity.**
      This is the assertion Task 3's whole design exists for
- [ ] `src = Pressure` moves smoothly at `slew ≈ 0.5` and audibly steps at `slew = 0`
- [ ] `src = CC`, `cc_num = 74` follows an external controller
- [ ] Eight routes enabled at different targets: no audible glitch, and `/system/cpu`
      shows no meaningful frame-budget change against a capture with all eight disabled
- [ ] `fx:move 3>1` with routes aimed at fx1 and fx3 re-aims both

**Verify:** the session transcript, with the CPU-page numbers for the 0-route and
8-route cases quoted **side by side** — two numbers, not "seems fine".

**Steps:**
- [ ] **Step 1: ASK before deploying.** And do not measure a device Charles is using.
- [ ] **Step 2:** `./scripts/build.sh`
- [ ] **Step 3:** `./scripts/install.sh local --skip-modules --skip-confirmation`
- [ ] **Step 4:** Work the acceptance list in order, capturing what each one did.
- [ ] **Step 5:** Record both CPU-page numbers.

---

### Task 10: Documentation

**Goal:** The war stories land in the subsystem docs with **one hook bullet each** in
`CLAUDE.md` — it is an INDEX, per `claude_md_split_into_subsystem_docs`.

**Files:** `docs/CHAIN.md`, `docs/SHADOW_UI.md`, `CLAUDE.md`,
`src/shared/help_content.json`; `docs/MODULES.md` and `../schwung-catalog-site/manual.html`
only if a module-facing contract or a user gesture changed (neither should have).

**Acceptance Criteria:**
- [ ] `CLAUDE.md` gains exactly two bullets, not prose
- [ ] `docs/CHAIN.md` documents: `MOD_SRC_LFO == 0` is why there is no migration; the
      latch is post-MIDI-FX at **both** synth-feed paths, and why; last-note/channel-wide
      by construction and why poly is the synth's job; the send-amount target is an
      offset and never a write
- [ ] `docs/SHADOW_UI.md` documents the page budget and `phase_offset`'s demotion
- [ ] `help_content.json` uses `children` — a wrong key is silently discarded
- [ ] `bash tests/host/test_widget_sheet.sh` passes with **no** regeneration. If it
      fails, a widget changed and that is a finding, not a chore

**Verify:** full `tests/host` suite silent

**Draft `CLAUDE.md` bullet (chain hook):**

```markdown
- **A mod route's SOURCE is a field, and `MOD_SRC_LFO` is 0 so there is no
  migration.** The eight slot routes (`mod1:`..`mod8:`, with `lfo1:`/`lfo2:` kept
  as a read-only alias for the two that existed) each pick LFO, Velocity,
  Pressure, CC or Note. The MIDI sources latch at **both** synth-feed paths — the
  same rule and the same reason as `synth:last_note`: an arpeggiator emits from
  `tick()`, so a latch on `v2_on_midi` alone never moves with an arp in the slot,
  silently. They are last-note and channel-wide **by construction**: the bus moves
  a PARAMETER through `set_param`, so poly velocity cannot be expressed here and
  is the synth's job. Master FX keeps two LFOs and gets no Source cell — it has no
  MIDI input to read — and `lfo_process_midi` takes an explicit count so the two
  counts cannot be confused into an overrun.
```

---

## Deferred (real follow-ups, deliberately not in this plan)

1. **Bus-insert FX as mod targets.** #453 took a slot from 8 audio FX positions to 72
   (Main + 8 buses × 8). `chain_mod.c`'s resolver still knows only `synth`, `fx1..8`,
   `midi_fx1..8`. `bus_route.h` names this to-do outright: *"a bus insert chain declares
   `hasLfos: false` … Restore it when a bus can actually be an LFO target."* Note
   `lfo_state_t.target` is `char[16]`, matched by `SEND_TARGET_KEY_LEN` — a truncated
   target compares unequal and **silently stops modulating**. `bus3:fx7` fits; not much
   more does.
2. **Global send FX as mod targets.** Those live shim-side (`shadow_chain_mgmt.c`,
   `send_fx_key.h`), not in `chain_instance_t`. The mod bus cannot reach them at all.
3. **`MAX_MOD_TARGETS` (32) against 72+ positions.** Heap-only, so cheap — but size it
   against the real topology rather than raising it reflexively.
4. **Cross-slot modulation.** Needs a shim-level bus. Genuinely a different feature.
