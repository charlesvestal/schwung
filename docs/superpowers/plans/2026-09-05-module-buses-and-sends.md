# Module Buses and Global Sends Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A module that can render its voices separately gains per-voice insert chains and two device-wide send buses, so a drum module can carry distortion on the kick, chorus into phaser on the hats, and a shared delay and reverb.

**Architecture:** Every chain in Schwung stays 8 positions. The module publishes a flat ordered `split_voices` list and an optional dlsym'd `move_plugin_render_split` that accumulates into possibly-aliased buffers, so voice grouping lives entirely chain-side and no module ever honours a routing parameter. The chain sums its buses to one stereo output, leaving `external_fx_mode` untouched; the shim owns two global send buses hosted exactly like Master FX.

**Tech Stack:** C11 (chain host `src/modules/chain/dsp/`, shim `src/schwung_shim.c`, `src/host/`), header-only pure units tested natively via `tests/host/`, QuickJS ES modules for the shadow UI (`src/shadow/*.mjs`).

**User decisions (already made):**
- "Chain host owns it" — module declares buses, Schwung instantiates every FX, owns the graph, UI, persistence, bypass, LFOs, presets.
- "hmm but you might want to group HH and OH into a bus. so maybe this is arbitrary" — a bus is a user-created SET of voices, many-to-one, editable; the module declares only which voices it can render apart.
- "Separate pools on top of the 8" — Main 8 + 4 buses x 8 per slot, not a repurposing of the existing 8.
- "i'd rather give flexibility and people can use lighter plugins or sends" — the CPU ceiling is accepted deliberately; do not add a throttle or a lower cap.
- "should sends be global, not per-module?" -> global. Two device-wide sends, not per-slot.
- "2 sends x 8-position chains" — same machinery as Master FX, three instances.
- "you can hit down on the chain editor to access a module's buses, but a bus can have N (8?) modules" — the down-gesture from the synth box; 8 positions per bus.
- "go on, it's fine" — stems 6/7 and the 4-bus cap approved.
- "1" (harvest, don't merge) — re-implement #121's send design on current `main` and credit it; do not attempt to rebase or merge #121.

**Spec:** `docs/superpowers/specs/2026-09-05-module-buses-and-sends-design.md`

**Prior art:** PR #121 (legsmechanical) built the global-send half and device-verified it. Its merge-base is 2026-03-04 with `main` 1696 commits ahead, so it is not mergeable. Tasks 6, 8, 9 and 11 re-implement its validated decisions on current `main`; Task 12 credits it. Its Move FX half is out of scope.

---

## File Structure

**New, pure and header-only** (the house idiom: logic worth testing lives in a
dependency-free header under `src/host/` so `tests/host/` can compile and RUN it
natively, because the shim and chain translation units cannot be built on the
dev machine):

| File | Responsibility |
|---|---|
| `src/host/bus_mix.h` | Voice->buffer table construction with aliasing, the clear-set mask, accumulate-with-clamp, send scaling |
| `src/host/bus_route.h` | `bus<N>:` key routing and voice-id -> index resolution, cap passed in as a parameter |
| `src/host/send_fx_key.h` | `send<N>:fx<M>:<param>` and `send<N>:<param>` routing, caps passed in |

**New UI module:**

| File | Responsibility |
|---|---|
| `src/shadow/shadow_ui_buses.mjs` | Bus list, voice assignment, bus chain entry. A sibling `.mjs`, not more of `shadow_ui.js` — that file is already ~14k lines |

**Modified:**

| File | Change |
|---|---|
| `src/modules/chain/dsp/chain_internal.h` | `SLOT_BUSES`, `slot_bus_t`, bus fields on `chain_instance_t` |
| `src/modules/chain/dsp/chain_host.c` | `split_voices` read, `render_split` dlsym, the bus render path, `bus<N>:` param dispatch |
| `src/modules/chain/dsp/chain_patch.c` | Bus persistence in the slot file |
| `src/host/shadow_chain_mgmt.h` | `SEND_BUSES`, `SEND_FX_SLOTS`, `shadow_send_fx_slots` |
| `src/host/shadow_chain_mgmt.c` | Send bus hosting, param routing, persistence, presets |
| `src/schwung_shim.c` | `send_accum`, the send chains, returns, A->B, stem dispatch |
| `src/host/shadow_sampler.h` / `.c` | `SAMPLER_STEM_COUNT` 5 -> 7, two send stems |
| `src/shadow/shadow_ui.js` | Down-gesture on the synth box, FX-bus picker, wiring to `shadow_ui_buses.mjs` |
| `src/shadow/shadow_ui_master_fx.mjs` | Becomes one bus the picker opens |
| `tests/host/Makefile` | Three new native test targets |

---

## Task 1: `bus_mix.h` — the routing maths

**Goal:** A dependency-free header holding every arithmetic decision the bus render path makes, provable natively.

**Files:**
- Create: `src/host/bus_mix.h`
- Create: `tests/host/test_bus_mix.c`
- Create: `tests/host/test_bus_mix.sh`
- Modify: `tests/host/Makefile` (add target)

**Acceptance Criteria:**
- [ ] Two voices assigned to one bus receive the SAME pointer (aliasing is the summing mechanism)
- [ ] A voice assigned to no bus receives the main pointer
- [ ] A voice assigned to a bus whose buffer is NULL (not yet allocated) receives the main pointer, not a crash
- [ ] The active-bus mask names exactly the buses at least one voice targets
- [ ] Accumulate saturates at +32767 / -32768 rather than wrapping
- [ ] Send level 0 leaves the destination byte-identical; level 127 is exactly unity
- [ ] No allocation, no I/O, no locks anywhere in the header

**Verify:** `bash tests/host/test_bus_mix.sh` -> prints `PASS` and exits 0

> **Amended after code review.** `bus_mix_active_mask` takes `bus_buf` as a
> parameter and both it and `bus_mix_build_table` resolve a voice's target
> through one shared `bus_mix_target()` helper. Without that they disagreed:
> the mask named a bus whose buffer was NULL while that bus's voices had
> fallen back to main, so a caller doing what the doc comment said — memset
> everything the mask names — would NULL-deref on the SPI callback. The
> committed header is the authority; the code below is the pre-review draft.

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_bus_mix.c`:

```c
/* Unit tests for bus_mix.h — the per-voice routing and mixing arithmetic.
 *
 * Header-only and pure, so it runs natively on the dev machine. The chain
 * translation unit that calls it cannot be built here, which is exactly how
 * arithmetic like this ends up shipped untested. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bus_mix.h"

#define N 8   /* samples per test buffer; frames*2 in the real path */

static void test_aliasing_is_the_summing_mechanism(void) {
    int16_t main_buf[N], b0[N], b1[N];
    int16_t *bus_buf[2] = { b0, b1 };
    /* voices: 0 and 1 -> bus 0, 2 -> bus 1, 3 -> main */
    int8_t voice_bus[4] = { 0, 0, 1, BUS_MIX_MAIN };
    int16_t *voice_out[4];

    bus_mix_build_table(voice_out, 4, voice_bus, main_buf, bus_buf, 2);

    assert(voice_out[0] == b0);
    assert(voice_out[1] == b0);          /* SAME pointer: HH and OH share a bus */
    assert(voice_out[2] == b1);
    assert(voice_out[3] == main_buf);
    printf("  aliasing: ok\n");
}

static void test_unassigned_and_unallocated_fall_to_main(void) {
    int16_t main_buf[N], b0[N];
    int16_t *bus_buf[2] = { b0, NULL };   /* bus 1 not allocated yet */
    int8_t voice_bus[3] = { BUS_MIX_MAIN, 1, 7 };  /* 7 is out of range */
    int16_t *voice_out[3];

    bus_mix_build_table(voice_out, 3, voice_bus, main_buf, bus_buf, 2);

    assert(voice_out[0] == main_buf);
    assert(voice_out[1] == main_buf);    /* lazy allocation must not crash */
    assert(voice_out[2] == main_buf);    /* out of range is not a trap */
    printf("  fallbacks: ok\n");
}

static void test_active_mask_names_the_clear_set(void) {
    int8_t voice_bus[5] = { 0, 0, 2, BUS_MIX_MAIN, 2 };
    uint32_t mask = 0;
    int n = bus_mix_active_mask(voice_bus, 5, 4, &mask);
    assert(n == 2);
    assert(mask == ((1u << 0) | (1u << 2)));
    printf("  active mask: ok\n");
}

static void test_accumulate_saturates(void) {
    int16_t dst[4] = { 32000,  -32000, 0,  100 };
    int16_t src[4] = {  2000,   -2000, 0, -100 };
    bus_mix_accumulate(dst, src, 4);
    assert(dst[0] == 32767);    /* clamped, not wrapped to a negative */
    assert(dst[1] == -32768);
    assert(dst[2] == 0);
    assert(dst[3] == 0);
    printf("  accumulate saturates: ok\n");
}

static void test_send_level_endpoints(void) {
    int16_t dst[3] = { 5, 6, 7 };
    const int16_t before[3] = { 5, 6, 7 };
    int16_t src[3] = { 1000, -1000, 32767 };

    bus_mix_send(dst, src, 3, 0);
    assert(memcmp(dst, before, sizeof(before)) == 0);   /* 0 is a true no-op */

    int16_t dst2[3] = { 0, 0, 0 };
    bus_mix_send(dst2, src, 3, BUS_MIX_SEND_LEVEL_MAX);
    assert(dst2[0] == 1000 && dst2[1] == -1000 && dst2[2] == 32767);  /* unity */

    int16_t dst3[3] = { 0, 0, 0 };
    bus_mix_send(dst3, src, 3, 64);
    assert(dst3[0] > 400 && dst3[0] < 600);            /* roughly half */
    printf("  send endpoints: ok\n");
}

static void test_bus_sum_equals_voice_sum(void) {
    /* The exact-sum property: routing voices through buses and summing back
     * must equal summing the voices directly, absent clamping. */
    int16_t main_buf[N] = {0}, b0[N] = {0}, b1[N] = {0};
    int16_t *bus_buf[2] = { b0, b1 };
    int8_t voice_bus[3] = { 0, 1, BUS_MIX_MAIN };
    int16_t *voice_out[3];
    bus_mix_build_table(voice_out, 3, voice_bus, main_buf, bus_buf, 2);

    int16_t v[3][N];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < N; j++) v[i][j] = (int16_t)(100 * (i + 1) + j);

    for (int i = 0; i < 3; i++) bus_mix_accumulate(voice_out[i], v[i], N);
    bus_mix_accumulate(main_buf, b0, N);
    bus_mix_accumulate(main_buf, b1, N);

    for (int j = 0; j < N; j++)
        assert(main_buf[j] == (int16_t)(v[0][j] + v[1][j] + v[2][j]));
    printf("  exact sum: ok\n");
}

int main(void) {
    printf("test_bus_mix:\n");
    test_aliasing_is_the_summing_mechanism();
    test_unassigned_and_unallocated_fall_to_main();
    test_active_mask_names_the_clear_set();
    test_accumulate_saturates();
    test_send_level_endpoints();
    test_bus_sum_equals_voice_sum();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cc -std=gnu11 -Wall -Wextra -Isrc/host tests/host/test_bus_mix.c -o /tmp/t
```
Expected: FAIL — `fatal error: 'bus_mix.h' file not found`

- [ ] **Step 3: Write the implementation**

Create `src/host/bus_mix.h`:

```c
/*
 * bus_mix.h — per-voice bus routing and mixing arithmetic.
 *
 * Header-only and dependency-free for the same reason as master_fx_key.h and
 * chain_key_index.h: so tests/host can compile and RUN it natively. Its caller
 * is chain_host.c's v2_render_block, a translation unit that cannot be built on
 * the dev machine, which is how arithmetic like this ships untested.
 *
 * THE ALIASING IS THE FEATURE. Two voices in one bus are handed the SAME
 * pointer, so their sum happens in the module's own accumulating render with no
 * mixing pass of ours at all, and a voice in no bus is handed the main buffer
 * so the sparse case costs literally nothing. Callers must therefore clear only
 * the DISTINCT buffers (bus_mix_active_mask) and modules must ACCUMULATE.
 *
 * Every function here runs on the SCHED_FIFO SPI callback: no allocation, no
 * I/O, no locks.
 */
#ifndef BUS_MIX_H
#define BUS_MIX_H

#include <stdint.h>
#include <stddef.h>

/* voice_bus[] entry meaning "this voice is not on any bus". */
#define BUS_MIX_MAIN (-1)

/* Send levels are 0..127 so they survive a MIDI CC round trip unchanged and
 * need no float in the audio path. 127 is exactly unity, not 127/128. */
#define BUS_MIX_SEND_LEVEL_MAX 127

/*
 * How many global send buses a slot can feed.
 *
 * IT LIVES HERE, not in shadow_chain_mgmt.h, because BOTH sides need it and
 * only one of them may include that header: the chain is a MODULE, dlopen'd
 * through plugin_api_v2, and a module reaching into a shim header is exactly
 * the coupling that produced breakbeat's ABI drift. bus_mix.h is the one
 * header both the chain and the shim include. shadow_chain_mgmt.h defines
 * SEND_BUSES from this and static-asserts they agree, so there is one number
 * with one definition and a build failure if that stops being true. */
#define BUS_MIX_SENDS 2

/*
 * Build the per-voice output table.
 *
 * voice_bus[i] is BUS_MIX_MAIN or a bus index. Entries alias deliberately.
 * A bus that is out of range, or whose buffer is NULL because it has not been
 * allocated yet (bus chains are allocated on demand, off the RT thread), falls
 * back to main_buf — the voice is still heard, just not separately.
 */
static inline void bus_mix_build_table(int16_t **voice_out, int n_voices,
                                       const int8_t *voice_bus,
                                       int16_t *main_buf,
                                       int16_t *const *bus_buf, int n_buses)
{
    for (int i = 0; i < n_voices; i++) {
        int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
        voice_out[i] = (b >= 0 && b < n_buses && bus_buf && bus_buf[b])
                     ? bus_buf[b] : main_buf;
    }
}

/*
 * Bitmask of the buses at least one voice targets — the set that must be
 * cleared before an accumulating render. Returns how many bits are set.
 *
 * Clearing by this mask rather than clearing all n_buses is what keeps an
 * unused bus free: an allocated-but-unrouted bus is never touched per frame.
 */
static inline int bus_mix_active_mask(const int8_t *voice_bus, int n_voices,
                                      int n_buses, uint32_t *out_mask)
{
    uint32_t m = 0;
    int n = 0;
    for (int i = 0; i < n_voices; i++) {
        int b = voice_bus ? voice_bus[i] : BUS_MIX_MAIN;
        if (b >= 0 && b < n_buses && !(m & (1u << b))) {
            m |= 1u << b;
            n++;
        }
    }
    if (out_mask) *out_mask = m;
    return n;
}

static inline int16_t bus_mix_clamp(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* dst += src, saturating. n is SAMPLES (frames * 2), not frames. */
static inline void bus_mix_accumulate(int16_t *dst, const int16_t *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = bus_mix_clamp((int32_t)dst[i] + (int32_t)src[i]);
}

/*
 * dst += src * (level / BUS_MIX_SEND_LEVEL_MAX), saturating.
 *
 * Level 0 returns without touching dst — a send at zero must cost nothing,
 * because most buses feed most sends at zero.
 */
static inline void bus_mix_send(int16_t *dst, const int16_t *src, int n, int level)
{
    if (level <= 0) return;
    if (level > BUS_MIX_SEND_LEVEL_MAX) level = BUS_MIX_SEND_LEVEL_MAX;
    for (int i = 0; i < n; i++) {
        int32_t scaled = ((int32_t)src[i] * (int32_t)level) / BUS_MIX_SEND_LEVEL_MAX;
        dst[i] = bus_mix_clamp((int32_t)dst[i] + scaled);
    }
}

#endif /* BUS_MIX_H */
```

- [ ] **Step 4: Write the shell wrapper**

Create `tests/host/test_bus_mix.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_bus_mix"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  tests/host/test_bus_mix.c \
  -o "$bin"

"$bin"
```

Then `chmod +x tests/host/test_bus_mix.sh`.

- [ ] **Step 5: Add the Makefile target**

In `tests/host/Makefile`, add to the `TARGETS` list (after `$(BUILD_DIR)/test_link_audio_backlog_trim`):

```make
	$(BUILD_DIR)/test_bus_mix
```

and a rule beside the other single-source rules. Note the header dependency —
without it, editing `bus_mix.h` leaves a stale binary and `make test` reports
PASS against the code you just changed:

```make
$(BUILD_DIR)/test_bus_mix: test_bus_mix.c ../../src/host/bus_mix.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)
```

- [ ] **Step 6: Run to verify it passes**

```bash
bash tests/host/test_bus_mix.sh
make -C tests/host test
```
Expected: `PASS` from the first; all green from the second.

- [ ] **Step 7: Prove the test can fail**

Temporarily change `bus_mix_clamp`'s `if (v > 32767) return 32767;` to `return (int16_t)v;`, re-run, confirm `test_accumulate_saturates` aborts, then revert. (A probe that measures the wrong thing reports green — mutate to prove it CAN fail. Note `rm` the binary first: ExtFS has 1-second mtime granularity and make may not rebuild.)

```bash
rm -f build/tests/test_bus_mix
```

- [ ] **Step 8: Commit**

```bash
git add src/host/bus_mix.h tests/host/test_bus_mix.c tests/host/test_bus_mix.sh tests/host/Makefile
git commit -m "bus_mix: the per-voice routing arithmetic, as a testable header

Aliased output pointers are the summing mechanism: two voices on one bus get
one pointer and the module's accumulating render does the mix. Clearing is by
active mask so an unrouted bus costs nothing per frame."
```

---

## Task 2: `bus_route.h` — key routing and voice resolution

**Goal:** `bus<N>:` parameter routing and voice-id -> index resolution, with the cap passed in rather than restated.

**Files:**
- Create: `src/host/bus_route.h`
- Create: `tests/host/test_bus_route.c`
- Create: `tests/host/test_bus_route.sh`
- Modify: `tests/host/Makefile`

**Acceptance Criteria:**
- [ ] `bus1:` .. `bus<cap>:` parse to 0-based indices; `bus<cap+1>:` is rejected
- [ ] `bus0` and `bus01` are rejected (leading zeros are not an id we emit)
- [ ] `bus12:x` parses as bus 12, not bus 1 — multi-digit indices are not read out of a single character
- [ ] A key with no `:` after the digits is rejected rather than routed with a garbage param
- [ ] An unmatched key returns 0 and leaves the out-params untouched — it must never fall through to bus 0
- [ ] Voice-id lookup returns the index on a hit and -1 on a miss or a NULL list
- [ ] The cap is a parameter; the header contains no copy of it

**Verify:** `bash tests/host/test_bus_route.sh` -> `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_bus_route.c`:

```c
/* Unit tests for bus_route.h.
 *
 * The cap comes in as -DTEST_SLOT_BUSES, read out of chain_internal.h by the
 * shell wrapper, so this covers whatever range buses actually run with rather
 * than a number restated here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bus_route.h"

#ifndef TEST_SLOT_BUSES
#error "TEST_SLOT_BUSES must be defined by the build"
#endif

static void test_full_range_parses(void) {
    for (int n = 1; n <= TEST_SLOT_BUSES; n++) {
        char key[32];
        snprintf(key, sizeof(key), "bus%d:fx1:cutoff", n);
        int bus = -99;
        const char *rest = NULL;
        assert(bus_route_param_key(key, TEST_SLOT_BUSES, &bus, &rest) == 1);
        assert(bus == n - 1);
        assert(strcmp(rest, "fx1:cutoff") == 0);
    }
    printf("  full range: ok\n");
}

static void test_past_the_cap_is_rejected_not_routed_to_zero(void) {
    char key[32];
    snprintf(key, sizeof(key), "bus%d:fx1:cutoff", TEST_SLOT_BUSES + 1);
    int bus = -99;
    const char *rest = (const char *)0x1;
    assert(bus_route_param_key(key, TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus == -99);                  /* untouched, NOT assigned slot 0 */
    assert(rest == (const char *)0x1);
    printf("  past cap rejected: ok\n");
}

static void test_malformed_ids(void) {
    int bus = -99;
    const char *rest = NULL;
    assert(bus_route_param_key("bus0:x",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus01:x", TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus1",    TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("bus1fx",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("busx:y",  TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key("fx1:cut", TEST_SLOT_BUSES, &bus, &rest) == 0);
    assert(bus_route_param_key(NULL,      TEST_SLOT_BUSES, &bus, &rest) == 0);
    printf("  malformed ids: ok\n");
}

static void test_multi_digit_is_not_read_from_one_char(void) {
    const char *end = NULL;
    assert(bus_route_parse_index("bus12:x", &end) == 12);
    assert(*end == ':');
    printf("  multi-digit: ok\n");
}

static void test_target_key_roundtrip(void) {
    char out[BUS_TARGET_KEY_LEN];
    for (int n = 1; n <= TEST_SLOT_BUSES; n++) {
        assert(bus_route_target(out, sizeof(out), n) == 1);
        const char *end = NULL;
        assert(bus_route_parse_index(out, &end) == n);
        assert(*end == '\0');
    }
    printf("  target roundtrip: ok\n");
}

static void test_voice_index_lookup(void) {
    const char *ids[4] = { "kick", "snare", "chh", "ohh" };
    assert(bus_voice_index(ids, 4, "kick") == 0);
    assert(bus_voice_index(ids, 4, "ohh")  == 3);
    assert(bus_voice_index(ids, 4, "ride") == -1);   /* orphan: module changed */
    assert(bus_voice_index(ids, 4, NULL)   == -1);
    assert(bus_voice_index(NULL, 0, "kick") == -1);
    printf("  voice lookup: ok\n");
}

int main(void) {
    printf("test_bus_route (cap=%d):\n", TEST_SLOT_BUSES);
    test_full_range_parses();
    test_past_the_cap_is_rejected_not_routed_to_zero();
    test_malformed_ids();
    test_multi_digit_is_not_read_from_one_char();
    test_target_key_roundtrip();
    test_voice_index_lookup();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cc -std=gnu11 -Isrc/host -DTEST_SLOT_BUSES=4 tests/host/test_bus_route.c -o /tmp/t
```
Expected: FAIL — `'bus_route.h' file not found`

- [ ] **Step 3: Write the implementation**

Create `src/host/bus_route.h`:

```c
/*
 * bus_route.h — "bus<N>:" parameter routing and voice-id resolution.
 *
 * Header-only and dependency-free so tests/host can run it natively; see
 * master_fx_key.h, whose failure modes this deliberately copies the fix for.
 * An unmatched key here returns 0 and leaves the out-params ALONE. It must
 * never fall through to bus 0: Master FX's handler had exactly that
 * else-branch, and an unmatched "fx5:cutoff" was not dropped but routed into
 * slot 0 with a garbage param key, writing to a different running module.
 *
 * The cap is a PARAMETER (bus_count). SLOT_BUSES is named once, in
 * chain_internal.h, and this file holds no copy of it.
 *
 * Pure: no allocation, no I/O, no locks. Called on the SPI callback.
 */
#ifndef BUS_ROUTE_H
#define BUS_ROUTE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Buffer for a formatted "bus%d" LFO target key. Matches lfo_state_t.target
 * (char[16]) for the same reason MASTER_FX_TARGET_KEY_LEN does: a truncated
 * target compares unequal and silently stops modulating. */
#define BUS_TARGET_KEY_LEN 16

/*
 * Parse a leading "bus<N>" with N a 1-based decimal index.
 *
 * Returns N (>= 1) and points *out_end at the first byte after the digits;
 * returns -1 on no match, leaving *out_end untouched. Leading zeros are
 * rejected, matching chain_key_index.h. Accumulation is clamped so a long
 * digit run cannot overflow into a plausible-looking index.
 */
static inline int bus_route_parse_index(const char *key, const char **out_end)
{
    if (!key) return -1;
    if (key[0] != 'b' || key[1] != 'u' || key[2] != 's') return -1;
    const char *p = key + 3;
    if (*p < '1' || *p > '9') return -1;   /* rejects "bus0" and "bus01" */
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return n;
}

/*
 * Route "bus<N>:<rest>" to a 0-based bus index and the remainder.
 *
 * Returns 1 on a match, 0 otherwise. On 0 the out-params are untouched.
 */
static inline int bus_route_param_key(const char *key, int bus_count,
                                      int *out_bus, const char **out_rest)
{
    const char *end = NULL;
    int n = bus_route_parse_index(key, &end);
    if (n < 1 || n > bus_count) return 0;
    if (!end || *end != ':') return 0;
    if (out_bus) *out_bus = n - 1;
    if (out_rest) *out_rest = end + 1;
    return 1;
}

/* Format the "bus%d" LFO target for a 1-based index. Returns 1 on success, 0
 * if it would not fit — never a truncated key. */
static inline int bus_route_target(char *out, size_t out_len, int bus_1based)
{
    if (!out || out_len == 0 || bus_1based < 1) return 0;
    int n = snprintf(out, out_len, "bus%d", bus_1based);
    return (n > 0 && (size_t)n < out_len) ? 1 : 0;
}

/*
 * Resolve a voice id to its index in the module's flat split_voices list.
 *
 * Returns -1 on a miss. A miss is not an error: a bus config stores ids so a
 * module that adds a voice in a later version does not silently re-point every
 * existing bus, which means an id can legitimately no longer exist. The caller
 * reports the orphan rather than guessing.
 */
static inline int bus_voice_index(const char *const *ids, int n_ids, const char *id)
{
    if (!ids || !id) return -1;
    for (int i = 0; i < n_ids; i++)
        if (ids[i] && strcmp(ids[i], id) == 0) return i;
    return -1;
}

#endif /* BUS_ROUTE_H */
```

- [ ] **Step 4: Write the shell wrapper**

Create `tests/host/test_bus_route.sh`, reading the cap out of the shipped header:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

hdr=src/modules/chain/dsp/chain_internal.h

# Read the cap out of the shipped header rather than restating it, so the
# per-bus cases track the range buses actually run with. bus_route.h takes
# bus_count as a parameter and holds no copy of the cap; this proves full
# coverage of whatever the shipped value is, plus rejection of the first past it.
cap=$(awk '/^#define SLOT_BUSES /{print $3}' "$hdr")
if [ -z "$cap" ]; then
  echo "FAIL: could not read SLOT_BUSES from $hdr" >&2
  exit 1
fi

bin="build/tests/test_bus_route"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  -DTEST_SLOT_BUSES="$cap" \
  tests/host/test_bus_route.c \
  -o "$bin"

"$bin"
```

`chmod +x tests/host/test_bus_route.sh`.

- [ ] **Step 5: Define the cap so the wrapper can read it**

In `src/modules/chain/dsp/chain_internal.h`, beside `MAX_AUDIO_FX`:

```c
/* Buses a slot can hold, BESIDE Main. Main is bus 0 and is implicit: it is
 * never created or deleted, holds every voice not assigned elsewhere, and its
 * insert chain IS the slot's existing main chain. So a slot holds up to
 * SLOT_BUSES + 1 mixing destinations and (SLOT_BUSES + 1) * MAX_AUDIO_FX
 * positions.
 *
 * Raising this should be a one-line change: all "bus<N>:" key routing goes
 * through bus_route.h with this passed in as bus_count, and every loop over
 * buses is bounded by this name. Read out of this line by
 * tests/host/test_bus_route.sh. The bitmask in bus_mix_active_mask is a
 * uint32_t, so 32 is the hard ceiling. */
#define SLOT_BUSES 4
_Static_assert(SLOT_BUSES > 0 && SLOT_BUSES <= 32,
               "SLOT_BUSES must fit bus_mix_active_mask's uint32_t");
```

- [ ] **Step 6: Add the Makefile target**

```make
$(BUILD_DIR)/test_bus_route: test_bus_route.c ../../src/host/bus_route.h ../../src/modules/chain/dsp/chain_internal.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -DTEST_SLOT_BUSES=$(SLOT_BUSES) $< -o $@ $(LDFLAGS)
```

and, beside the existing `MASTER_FX_SLOTS` extraction near the top of the Makefile:

```make
SLOT_BUSES := $(shell awk '/^\#define SLOT_BUSES /{print $$3}' ../../src/modules/chain/dsp/chain_internal.h)
```

Add `$(BUILD_DIR)/test_bus_route` to `TARGETS`.

- [ ] **Step 7: Run to verify it passes**

```bash
bash tests/host/test_bus_route.sh && make -C tests/host test
```
Expected: `PASS`, then all green.

- [ ] **Step 8: Prove it can fail**

Change `if (n < 1 || n > bus_count) return 0;` to `if (n < 1) return 0;`, `rm -f build/tests/test_bus_route`, re-run, confirm `test_past_the_cap_is_rejected_not_routed_to_zero` aborts, revert.

- [ ] **Step 9: Commit**

```bash
git add src/host/bus_route.h tests/host/test_bus_route.c tests/host/test_bus_route.sh tests/host/Makefile src/modules/chain/dsp/chain_internal.h
git commit -m "bus_route: bus<N>: key routing with the cap as a parameter

An unmatched key returns 0 and leaves the out-params alone rather than
falling through to bus 0 — the Master FX handler's else-branch assigned
slot 0 and wrote a garbage param key into a different running module."
```

---

## Task 3: The `split_voices` contract in the chain host

**Goal:** The chain reads the synth's flat `split_voices` list at load, holds the ids, and answers `synth:split_voices` for the UI.

**Files:**
- Modify: `src/modules/chain/dsp/chain_internal.h` (voice table on `chain_instance_t`)
- Modify: `src/modules/chain/dsp/chain_host.c` (read at load, clear at unload, expose)
- Create: `tests/host/test_split_voices_parse.c`
- Create: `tests/host/test_split_voices_parse.sh`
- Modify: `tests/host/Makefile`

**Acceptance Criteria:**
- [ ] A synth publishing `split_voices` has its ids parsed into a flat ordered table; index in the JSON is index in the table
- [ ] A synth publishing no `split_voices` leaves the table empty and `split_voice_count` 0
- [ ] The table is cleared on instance create AND on every synth load, so an id from the previous module can never name a voice in a list that no longer exists
- [ ] A `null` answer (read failed) and an `""` answer (key unserved) are told apart: neither produces a table, and the failed read is retried rather than latched as "this module cannot split"
- [ ] Ids longer than the buffer are rejected, not truncated — a truncated id would compare unequal and orphan the bus silently
- [ ] Parsing is bounded: at most `SPLIT_VOICES_MAX` entries

**Verify:** `bash tests/host/test_split_voices_parse.sh` -> `PASS`

**Steps:**

- [ ] **Step 1: Add the storage**

In `chain_internal.h`, beside the other synth fields on `chain_instance_t`:

```c
/* Voices this synth can render into separate buffers, in the module's own
 * declared order — the index here IS the voice_out[] index handed to
 * move_plugin_render_split.
 *
 * FLAT AND ORDERED ON PURPOSE. The bus->voice map has to be resolved in C on
 * the SPI callback, and chain_json.c's helpers are flat key scans that cannot
 * walk ui_hierarchy's `levels` in order — the same constraint that makes
 * synth:last_note report a note rather than a voice index. So the module
 * publishes a flat array and we never try to walk its hierarchy here.
 *
 * Reset on create and on every synth load: an id left over from the previous
 * module must not name a voice in a list that no longer exists. */
#define SPLIT_VOICES_MAX 32
#define SPLIT_VOICE_ID_LEN 32
char synth_split_voice_ids[SPLIT_VOICES_MAX][SPLIT_VOICE_ID_LEN];
int  synth_split_voice_count;
/* The read did not complete (claim refused / timed out), as opposed to
 * completing with no voices. Never latch a plan on this — retry. */
int  synth_split_read_failed;
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_split_voices_parse.c`. It links the parser directly, so
the parser must be a standalone function in a header the test can include —
put it in `src/host/split_voices_parse.h`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "split_voices_parse.h"

static void test_flat_order_is_the_buffer_index(void) {
    char ids[8][32];
    int n = split_voices_parse(
        "[{\"id\":\"kick\",\"label\":\"Kick\"},"
        "{\"id\":\"snare\",\"label\":\"Snare\"},"
        "{\"id\":\"chh\",\"label\":\"Closed Hat\"}]",
        ids, 8, 32);
    assert(n == 3);
    assert(strcmp(ids[0], "kick") == 0);
    assert(strcmp(ids[1], "snare") == 0);
    assert(strcmp(ids[2], "chh") == 0);
    printf("  flat order: ok\n");
}

static void test_absent_and_failed_are_not_the_same(void) {
    char ids[8][32];
    /* "" — the channel served us, the key produced nothing. */
    assert(split_voices_parse("", ids, 8, 32) == 0);
    /* "[]" — the module says it has no splittable voices. */
    assert(split_voices_parse("[]", ids, 8, 32) == 0);
    /* NULL — the read did not complete. A distinct return, so the caller can
     * retry instead of concluding the module cannot split. */
    assert(split_voices_parse(NULL, ids, 8, 32) == SPLIT_VOICES_READ_FAILED);
    printf("  tri-state: ok\n");
}

static void test_overlong_id_is_rejected_not_truncated(void) {
    char ids[8][8];   /* deliberately tiny */
    int n = split_voices_parse("[{\"id\":\"kick\"},{\"id\":\"a_very_long_voice_id\"}]",
                               ids, 8, 8);
    assert(n == 1);                       /* the long one is dropped */
    assert(strcmp(ids[0], "kick") == 0);
    printf("  overlong rejected: ok\n");
}

static void test_bounded(void) {
    char big[4096] = "[";
    for (int i = 0; i < 40; i++) {
        char one[64];
        snprintf(one, sizeof(one), "%s{\"id\":\"v%d\"}", i ? "," : "", i);
        strcat(big, one);
    }
    strcat(big, "]");
    char ids[8][32];
    int n = split_voices_parse(big, ids, 8, 32);
    assert(n == 8);                       /* capped at max_ids, no overrun */
    printf("  bounded: ok\n");
}

int main(void) {
    printf("test_split_voices_parse:\n");
    test_flat_order_is_the_buffer_index();
    test_absent_and_failed_are_not_the_same();
    test_overlong_id_is_rejected_not_truncated();
    test_bounded();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run to verify it fails**

```bash
cc -std=gnu11 -Isrc/host tests/host/test_split_voices_parse.c -o /tmp/t
```
Expected: FAIL — `'split_voices_parse.h' file not found`

- [ ] **Step 4: Write the parser**

Create `src/host/split_voices_parse.h`:

```c
/*
 * split_voices_parse.h — extract the flat ordered voice-id list a module
 * publishes as get_param("split_voices").
 *
 * Header-only so tests/host can run it; called from chain_host.c on the SPI
 * callback at synth-load time, so: no allocation, no I/O, bounded scan.
 *
 * A THREE-ANSWER READ. Callers must branch on the RAW value before parsing:
 *   JSON  the module answered
 *   ""    the channel served us, the key produced nothing (no split support)
 *   NULL  the read did not complete — SPLIT_VOICES_READ_FAILED
 * Collapsing NULL into "" is what makes a timed-out read latch as a verdict.
 */
#ifndef SPLIT_VOICES_PARSE_H
#define SPLIT_VOICES_PARSE_H

#include <stddef.h>
#include <string.h>

#define SPLIT_VOICES_READ_FAILED (-1)

/*
 * Parse [{"id":"kick",...},...] into ids[0..n). Returns the count, or
 * SPLIT_VOICES_READ_FAILED if json is NULL.
 *
 * An entry whose id does not fit id_len is SKIPPED, not truncated: a truncated
 * id compares unequal to the one stored in a bus config and would orphan the
 * bus with no way to tell why.
 */
static inline int split_voices_parse(const char *json, void *ids_void,
                                     int max_ids, int id_len)
{
    if (!json) return SPLIT_VOICES_READ_FAILED;
    char (*ids)[1] = (char (*)[1])ids_void;
    int n = 0;
    const char *p = json;
    while (*p && n < max_ids) {
        const char *k = strstr(p, "\"id\"");
        if (!k) break;
        k += 4;
        while (*k == ' ' || *k == ':') k++;
        if (*k != '"') { p = k; continue; }
        k++;
        const char *end = strchr(k, '"');
        if (!end) break;
        int len = (int)(end - k);
        if (len > 0 && len < id_len) {
            char *dst = (char *)ids + (size_t)n * (size_t)id_len;
            memcpy(dst, k, (size_t)len);
            dst[len] = '\0';
            n++;
        }
        p = end + 1;
    }
    return n;
}

#endif /* SPLIT_VOICES_PARSE_H */
```

- [ ] **Step 5: Wire it into the chain host**

In `chain_host.c`, in `v2_load_synth` immediately after the synth instance is
created (and in `v2_create_instance`), reset then populate:

```c
/* Reset FIRST, unconditionally: an id from the previous module must never
 * name a voice in a list that no longer exists — the same rule as
 * synth_last_note = -1 on every synth load. */
memset(inst->synth_split_voice_ids, 0, sizeof(inst->synth_split_voice_ids));
inst->synth_split_voice_count = 0;
inst->synth_split_read_failed = 0;

if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->get_param) {
    char buf[4096];
    buf[0] = '\0';
    int got = inst->synth_plugin_v2->get_param(inst->synth_instance,
                                               "split_voices", buf, sizeof(buf));
    /* got <= 0 means the key was not served: the module has no split support.
     * That is a real answer, distinct from a read that did not complete. */
    if (got > 0) {
        int n = split_voices_parse(buf, inst->synth_split_voice_ids,
                                   SPLIT_VOICES_MAX, SPLIT_VOICE_ID_LEN);
        if (n == SPLIT_VOICES_READ_FAILED) inst->synth_split_read_failed = 1;
        else inst->synth_split_voice_count = n;
    }
}
```

Add `#include "split_voices_parse.h"` at the top of `chain_host.c`.

- [ ] **Step 6: Expose it to the UI**

In `v2_get_param`'s synth branch, add a `synth:split_voices` case that
re-serves the module's own answer verbatim (the UI needs the labels, which we
do not store):

```c
if (strcmp(key, "synth:split_voices") == 0) {
    if (!(inst->synth_plugin_v2 && inst->synth_instance &&
          inst->synth_plugin_v2->get_param)) return 0;
    return inst->synth_plugin_v2->get_param(inst->synth_instance,
                                            "split_voices", buf, buf_len);
}
```

- [ ] **Step 7: Shell wrapper + Makefile target**

`tests/host/test_split_voices_parse.sh` follows Task 1's shape (no cap needed).
Makefile rule:

```make
$(BUILD_DIR)/test_split_voices_parse: test_split_voices_parse.c ../../src/host/split_voices_parse.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)
```

- [ ] **Step 8: Run to verify it passes**

```bash
bash tests/host/test_split_voices_parse.sh && make -C tests/host test
```
Expected: `PASS`, all green.

- [ ] **Step 9: Commit**

```bash
git add src/host/split_voices_parse.h tests/host/test_split_voices_parse.* tests/host/Makefile src/modules/chain/dsp/chain_internal.h src/modules/chain/dsp/chain_host.c
git commit -m "chain: read the synth's flat split_voices list at load

Flat and ordered because the bus->voice map resolves in C on the callback and
chain_json.c cannot walk ui_hierarchy's levels in order. Reset on every synth
load so an id from the previous module cannot name a voice that is gone."
```

---

## Task 4: `render_split` discovery and the bus render path

**Goal:** `v2_render_block` renders per-voice into aliased bus buffers, runs each bus's inserts, and sums back — falling back byte-identically to today's path when the module has no split.

**Files:**
- Modify: `src/modules/chain/dsp/chain_internal.h` (bus buffers, `render_split` fn pointer)
- Modify: `src/modules/chain/dsp/chain_host.c` (`v2_load_synth` dlsym, `v2_render_block`)

**Acceptance Criteria:**
- [ ] `move_plugin_render_split` is discovered by `dlsym` on the synth handle, NOT added as a field to `plugin_api_v2_t`
- [ ] A module exporting no such symbol renders through the existing `render_block` path with a byte-identical result
- [ ] Only the DISTINCT bus buffers named by `bus_mix_active_mask` are cleared each frame
- [ ] Bus inserts run in series per bus, honouring per-position bypass exactly as the main chain does
- [ ] Buses sum into the main buffer BEFORE the main chain's 8 FX
- [ ] `external_fx_mode` still returns raw synth output with no bus FX applied downstream of the shim's split
- [ ] Nothing in the added path allocates, logs or does I/O

**Verify:** `./scripts/build.sh` succeeds; `bash tests/host/test_chain_host_v2_only.sh` and `make -C tests/host test` stay green

**Steps:**

- [ ] **Step 1: Add the storage and the pointer**

In `chain_internal.h` on `chain_instance_t`:

```c
/* Optional per-voice render, discovered by dlsym on the synth handle.
 *
 * A SEPARATE EXPORTED SYMBOL, NOT A FIELD ON plugin_api_v2_t. Appending to
 * that struct is what boot-looped a device via breakbeat's header drift: a
 * module cannot extend the ABI from its side, and a guarded read of a field
 * we do not have tests memory belonging to somebody else. A dlsym'd symbol is
 * absent-or-present with no offset to get wrong. */
void (*synth_render_split)(void *instance, int16_t *const *voice_out,
                           int n_voices, int frames);

/* Per-bus insert chains and their mix buffers. Allocated ON DEMAND (Task 5),
 * so a NULL buffer is normal and bus_mix_build_table routes past it to main. */
typedef struct {
    int   in_use;
    char  name[MAX_NAME_LEN];
    int16_t *buf;                                  /* FRAMES_PER_BLOCK * 2 */
    void *fx_handles[MAX_AUDIO_FX];
    audio_fx_api_v2_t *fx_plugins_v2[MAX_AUDIO_FX];
    void *fx_instances[MAX_AUDIO_FX];
    int   fx_bypassed[MAX_AUDIO_FX];
    int   fx_count;
    char  current_fx_modules[MAX_AUDIO_FX][MAX_NAME_LEN];
    int   send_level[BUS_MIX_SENDS];              /* 0..127, see bus_mix.h */
} slot_bus_t;

slot_bus_t buses[SLOT_BUSES];
/* voice index -> bus index, or BUS_MIX_MAIN. Resolved from stored ids at
 * synth load and whenever a bus's voice set changes. */
int8_t voice_bus[SPLIT_VOICES_MAX];
int    main_send_level[BUS_MIX_SENDS];  /* Main is bus 0 and sends like any bus */
```

Add `#include "bus_mix.h"` and `#include "bus_route.h"` to `chain_internal.h`.

- [ ] **Step 2: Discover the symbol at load**

In `v2_load_synth`, right after the `dlopen` handle is obtained and beside the
existing `dlinfo` base logging (which is what turns a raw `lr` into an
`addr2line` offset under ASLR — keep it):

```c
/* Optional. dlsym returning NULL is the normal case, not an error. */
inst->synth_render_split = (void (*)(void *, int16_t *const *, int, int))
    dlsym(handle, "move_plugin_render_split");
```

and clear it to NULL on unload and in `v2_create_instance`.

- [ ] **Step 3: Replace the synth render in `v2_render_block`**

In `chain_host.c:2029` `v2_render_block`, replace the existing synth-render
block (the `if (inst->synth_plugin_v2 && ... render_block) { ... } else { memset }`
plus the `synth_bypassed` memset) with:

```c
    /* Per-voice render when the module supports it AND a bus is actually
     * routed. Both conditions matter: with no bus routed, every voice_out[]
     * entry would be main_buf and the split render is the plain render with
     * extra steps. */
    /* Snapshot the buffers FIRST: the mask depends on them. A bus whose buffer
     * has not been allocated yet is not "active", because its voices fall back
     * to main — bus_mix_target is the single resolve both answers come from,
     * so the mask can never name a buffer nobody rendered into. */
    int16_t *bus_bufs[SLOT_BUSES];
    for (int b = 0; b < SLOT_BUSES; b++)
        bus_bufs[b] = inst->buses[b].buf;

    uint32_t active_bus_mask = 0;
    int n_active = 0;
    if (inst->synth_render_split && inst->synth_split_voice_count > 0) {
        n_active = bus_mix_active_mask(inst->voice_bus,
                                       inst->synth_split_voice_count,
                                       SLOT_BUSES, bus_bufs, &active_bus_mask);
    }

    if (n_active > 0) {
        /* Clear the main buffer and ONLY the buses the mask names. An
         * allocated-but-unrouted bus is never touched. Every bit in the mask
         * has a non-NULL buffer by construction, so no NULL check is needed
         * here — but do not add one back "defensively": it would hide a mask
         * that had started lying. */
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        for (int b = 0; b < SLOT_BUSES; b++) {
            if (active_bus_mask & (1u << b))
                memset(bus_bufs[b], 0, frames * 2 * sizeof(int16_t));
        }

        int16_t *voice_out[SPLIT_VOICES_MAX];
        bus_mix_build_table(voice_out, inst->synth_split_voice_count,
                            inst->voice_bus, out_interleaved_lr,
                            bus_bufs, SLOT_BUSES);

        /* ACCUMULATES. The buffers above are the cleared ones. */
        inst->synth_render_split(inst->synth_instance, voice_out,
                                 inst->synth_split_voice_count, frames);

        /* Per-bus inserts, in series, with the same bypass discipline as the
         * main chain: always process so delay lines and reverb tails advance,
         * restore the dry on a bypassed position so unbypass resumes cleanly. */
        for (int b = 0; b < SLOT_BUSES; b++) {
            if (!(active_bus_mask & (1u << b))) continue;
            slot_bus_t *bus = &inst->buses[b];
            for (int i = 0; i < bus->fx_count && i < MAX_AUDIO_FX; i++) {
                int bypassed = bus->fx_bypassed[i];
                int16_t dry[FRAMES_PER_BLOCK * 2];
                if (bypassed)
                    memcpy(dry, bus_bufs[b], frames * 2 * sizeof(int16_t));
                if (bus->fx_plugins_v2[i] && bus->fx_instances[i] &&
                    bus->fx_plugins_v2[i]->process_block) {
                    bus->fx_plugins_v2[i]->process_block(bus->fx_instances[i],
                                                         bus_bufs[b], frames);
                }
                if (bypassed)
                    memcpy(bus_bufs[b], dry, frames * 2 * sizeof(int16_t));
            }
        }

        /* Buses sum into main BEFORE the main chain's 8 FX, so a slot
         * compressor sees the whole kit. Sends are taken in Task 6, from the
         * post-insert bus buffers, which are still intact here. */
        for (int b = 0; b < SLOT_BUSES; b++) {
            if (!(active_bus_mask & (1u << b))) continue;
            bus_mix_accumulate(out_interleaved_lr, bus_bufs[b], frames * 2);
        }
    } else if (inst->synth_plugin_v2 && inst->synth_instance &&
               inst->synth_plugin_v2->render_block) {
        inst->synth_plugin_v2->render_block(inst->synth_instance,
                                            out_interleaved_lr, frames);
    } else {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
    }

    if (inst->synth_bypassed) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
    }
```

Note what is preserved: the render happens even when bypassed (so envelopes,
LFOs and phases keep advancing and unbypass resumes without a burst), and the
zeroing happens after.

- [ ] **Step 4: Build for the device**

```bash
./scripts/build.sh
```
Expected: succeeds. This is a cross-compile in Docker and is the only thing
that compiles the chain translation unit at all.

- [ ] **Step 5: Confirm the fallback is untouched**

```bash
bash tests/host/test_chain_host_v2_only.sh
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```
Expected: all green. CI gates exactly this set.

- [ ] **Step 6: Commit**

```bash
git add src/modules/chain/dsp/chain_internal.h src/modules/chain/dsp/chain_host.c
git commit -m "chain: render per-voice into aliased bus buffers

render_split is a dlsym'd symbol, not a field on plugin_api_v2_t — a module
cannot extend that ABI from its side, and appending to it is what boot-looped
a device via breakbeat's header drift. Only the distinct bus buffers named by
the active mask are cleared, so an unrouted bus costs nothing per frame."
```

---

## Task 5: On-demand bus allocation, off the RT thread

**Goal:** A bus's chain and mix buffer are allocated when the bus is created, on a SCHED_OTHER worker, and published to the audio path by pointer.

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (creation path + worker)
- Modify: `src/modules/chain/dsp/chain_internal.h` (request state)

**Acceptance Criteria:**
- [ ] No bus allocation happens on the SPI callback
- [ ] The worker demotes itself to SCHED_OTHER and pins to cores 0-2 as its FIRST action
- [ ] A bus with no buffer yet renders through Main (Task 1 already proves the fallback) rather than dropping audio
- [ ] Slot memory is unchanged from today until a bus exists
- [ ] Destroying an instance frees every bus buffer and chain

**Verify:** `./scripts/build.sh`; on device, create a bus and confirm `tests/host/test_spi_path_rt_hygiene.sh`-style grep finds no `calloc`/`malloc` reachable from the bus creation call on the callback

**Steps:**

- [ ] **Step 1: Add the request state**

In `chain_internal.h`:

```c
/* Bus creation is a REQUEST, not an action. create_instance, set_param and
 * every other module entry point run on the SPI callback (FIFO, ~2370 us
 * frame budget), and a bus costs ~9.1 MB: 8 positions of chain_param_info_t
 * at ~1.07 MB plus 8 x 64 KB of cached ui_hierarchy. Allocating that inline
 * is a multi-megabyte calloc inside the audio thread.
 *
 * The RT side sets pending and returns; the worker allocates and publishes
 * buf and the metadata pointers; the RT side sees them appear. */
volatile int bus_alloc_pending[SLOT_BUSES];
pthread_t bus_worker;
int bus_worker_started;
```

- [ ] **Step 2: Write the worker**

In `chain_host.c`:

```c
/*
 * Bus allocation worker. SCHED_OTHER on cores 0-2.
 *
 * THREADS INHERIT THE CALLBACK'S PRIORITY. pthread_create from any module
 * entry point hands the worker SCHED_FIFO, and Move's own Link Main runs at
 * FIFO 35 — an inherited-priority worker starves Move's audio publisher and
 * produces exactly the dropouts going off-thread was meant to avoid. Demote
 * FIRST, before anything else.
 */
static void *chain_bus_worker_fn(void *arg) {
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
    sched_setaffinity(0, sizeof(set), &set);

    chain_instance_t *inst = (chain_instance_t *)arg;
    while (inst->bus_worker_started) {
        for (int b = 0; b < SLOT_BUSES; b++) {
            if (!inst->bus_alloc_pending[b]) continue;
            slot_bus_t *bus = &inst->buses[b];
            if (!bus->buf) {
                int16_t *buf = (int16_t *)calloc(FRAMES_PER_BLOCK * 2, sizeof(int16_t));
                if (buf) {
                    /* Publish LAST: the RT side reads buf and, seeing it
                     * non-NULL, starts routing voices into it. Everything it
                     * will touch must already be valid. */
                    __atomic_store_n(&bus->buf, buf, __ATOMIC_RELEASE);
                }
            }
            inst->bus_alloc_pending[b] = 0;
        }
        usleep(20000);
    }
    return NULL;
}
```

Start it lazily on the first bus creation (not in `create_instance`, so a slot
with no buses starts no thread), and join it in `v2_destroy_instance` before
freeing.

- [ ] **Step 3: Request from the RT side**

In the `bus<N>:create` param handler (Task 8 adds the dispatch):

```c
/* RT side: mark and return. No allocation here. */
inst->buses[b].in_use = 1;
inst->bus_alloc_pending[b] = 1;
if (!inst->bus_worker_started) {
    inst->bus_worker_started = 1;
    pthread_create(&inst->bus_worker, NULL, chain_bus_worker_fn, inst);
}
```

- [ ] **Step 4: Free on destroy**

In `v2_destroy_instance`, before the existing frees:

```c
if (inst->bus_worker_started) {
    inst->bus_worker_started = 0;
    pthread_join(inst->bus_worker, NULL);
}
for (int b = 0; b < SLOT_BUSES; b++) {
    free(inst->buses[b].buf);
    inst->buses[b].buf = NULL;
    /* unload each bus FX position exactly as the main chain's are unloaded */
}
```

- [ ] **Step 5: Build and check RT hygiene**

```bash
./scripts/build.sh
bash tests/shadow/test_spi_path_rt_hygiene.sh || true   # not CI-gated; read the output
```

- [ ] **Step 6: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c src/modules/chain/dsp/chain_internal.h
git commit -m "chain: allocate bus chains on demand, on a demoted worker

A bus is ~9.1 MB and create_instance runs on the SPI callback. The worker
demotes to SCHED_OTHER and pins to cores 0-2 as its first action: an
inherited-FIFO worker starves Move's Link Main at 35."
```

---

## Task 6: Global send buses in the shim

**Goal:** Two device-wide send buses hosted exactly like Master FX, fed by every bus in every slot, with return levels and a feedback-safe A->B.

**Files:**
- Create: `src/host/send_fx_key.h`
- Create: `tests/host/test_send_fx_key.c`, `tests/host/test_send_fx_key.sh`
- Modify: `tests/host/Makefile`
- Modify: `src/host/shadow_chain_mgmt.h` (`SEND_BUSES`, `SEND_FX_SLOTS`, storage)
- Modify: `src/host/shadow_chain_mgmt.c` (hosting, param routing)
- Modify: `src/schwung_shim.c` (`send_accum`, chains, returns, A->B)

**Acceptance Criteria:**
- [ ] `send1:fx3:cutoff` routes to send 0, slot 2, param `cutoff`; `send1:return` routes to send 0 with slot -1
- [ ] `send<SEND_BUSES+1>:` and `send1:fx<SEND_FX_SLOTS+1>:` are both rejected, out-params untouched
- [ ] Send chains are `master_fx_slot_t` arrays — Master FX's hosting, bypass and preset machinery is reused, not duplicated
- [ ] A->B is applied after A's chain and before B's, so B can never reach A
- [ ] A send with no FX loaded and no level costs no `process_block` calls
- [ ] Returns sum into the shadow mix BEFORE Master FX

**Verify:** `bash tests/host/test_send_fx_key.sh` -> `PASS`; `./scripts/build.sh` succeeds

**Steps:**

- [ ] **Step 1: Declare the caps**

In `shadow_chain_mgmt.h`, beside `MASTER_FX_SLOTS`:

Add `#include "bus_mix.h"` to `shadow_chain_mgmt.h` first, then:

```c
/* Global send buses and their chain depth.
 *
 * Eight positions, the same as Master FX: every chain in Schwung is 8
 * positions, so all three (Master, Send A, Send B) are the same machinery and
 * the same editor. Raising either should be a one-line change — all
 * "send<N>:fx<M>:" routing goes through send_fx_key.h with these passed in,
 * and every loop is bounded by these names.
 *
 * Design credit: PR #121 (legsmechanical), which established the send
 * topology, the post-fader rule, return levels, A->B and shared presets.
 * Re-implemented here because that branch's merge-base is 2026-03-04. */
#define SEND_BUSES BUS_MIX_SENDS
#define SEND_FX_SLOTS MASTER_FX_SLOTS

/* The chain sizes its per-bus send arrays from BUS_MIX_SENDS and cannot see
 * this header, so the two must not drift. One number, one definition. */
_Static_assert(SEND_BUSES == BUS_MIX_SENDS,
               "SEND_BUSES must equal BUS_MIX_SENDS -- the chain sizes from bus_mix.h");
_Static_assert(SEND_BUSES > 0 && SEND_BUSES <= 9999,
               "SEND_BUSES must fit \"send%d\" in SEND_TARGET_KEY_LEN");
```

- [ ] **Step 2: Write the failing test**

`tests/host/test_send_fx_key.c`, mirroring Task 2's structure:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "send_fx_key.h"

#ifndef TEST_SEND_BUSES
#error "TEST_SEND_BUSES must be defined by the build"
#endif
#ifndef TEST_SEND_FX_SLOTS
#error "TEST_SEND_FX_SLOTS must be defined by the build"
#endif

static void test_full_range(void) {
    for (int s = 1; s <= TEST_SEND_BUSES; s++) {
        for (int f = 1; f <= TEST_SEND_FX_SLOTS; f++) {
            char key[48];
            snprintf(key, sizeof(key), "send%d:fx%d:cutoff", s, f);
            int send = -99, slot = -99;
            const char *param = NULL;
            assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                                 &send, &slot, &param) == 1);
            assert(send == s - 1);
            assert(slot == f - 1);
            assert(strcmp(param, "cutoff") == 0);
        }
    }
    printf("  full range: ok\n");
}

static void test_bus_level_keys(void) {
    int send = -99, slot = -99;
    const char *param = NULL;
    assert(send_fx_route("send1:return", TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                         &send, &slot, &param) == 1);
    assert(send == 0);
    assert(slot == -1);                        /* a bus-level key, not an FX */
    assert(strcmp(param, "return") == 0);
    printf("  bus-level keys: ok\n");
}

static void test_past_caps_rejected(void) {
    int send = -99, slot = -99;
    const char *param = (const char *)0x1;
    char key[48];

    snprintf(key, sizeof(key), "send%d:return", TEST_SEND_BUSES + 1);
    assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                         &send, &slot, &param) == 0);
    assert(send == -99 && slot == -99 && param == (const char *)0x1);

    snprintf(key, sizeof(key), "send1:fx%d:cutoff", TEST_SEND_FX_SLOTS + 1);
    assert(send_fx_route(key, TEST_SEND_BUSES, TEST_SEND_FX_SLOTS,
                         &send, &slot, &param) == 0);
    assert(send == -99 && slot == -99);
    printf("  past caps rejected: ok\n");
}

static void test_malformed(void) {
    int send, slot; const char *param;
    assert(send_fx_route("send0:return", TEST_SEND_BUSES, TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    assert(send_fx_route("send1",        TEST_SEND_BUSES, TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    assert(send_fx_route("fx1:cutoff",   TEST_SEND_BUSES, TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    assert(send_fx_route(NULL,           TEST_SEND_BUSES, TEST_SEND_FX_SLOTS, &send, &slot, &param) == 0);
    printf("  malformed: ok\n");
}

int main(void) {
    printf("test_send_fx_key (buses=%d slots=%d):\n", TEST_SEND_BUSES, TEST_SEND_FX_SLOTS);
    test_full_range();
    test_bus_level_keys();
    test_past_caps_rejected();
    test_malformed();
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run to verify it fails**

```bash
cc -std=gnu11 -Isrc/host -DTEST_SEND_BUSES=2 -DTEST_SEND_FX_SLOTS=8 tests/host/test_send_fx_key.c -o /tmp/t
```
Expected: FAIL — `'send_fx_key.h' file not found`

- [ ] **Step 4: Write the header**

Create `src/host/send_fx_key.h`:

```c
/*
 * send_fx_key.h — "send<N>:fx<M>:<param>" and "send<N>:<param>" routing.
 *
 * Header-only and dependency-free, like master_fx_key.h, whose caps-as-
 * parameters discipline this follows: SEND_BUSES and SEND_FX_SLOTS are named
 * once in shadow_chain_mgmt.h and this file holds no copy.
 *
 * An unmatched key returns 0 with the out-params untouched. It must never
 * default a send or a slot: the Master FX param handler's else-branch assigned
 * slot 0, so an out-of-range key was routed INTO a running module with a
 * garbage param name rather than dropped.
 */
#ifndef SEND_FX_KEY_H
#define SEND_FX_KEY_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "master_fx_key.h"   /* master_fx_parse_index, for the "fx<M>" half */

#define SEND_TARGET_KEY_LEN 16

/* Parse a leading "send<N>", N 1-based. Returns N or -1; *out_end points past
 * the digits on success. Leading zeros rejected. */
static inline int send_fx_parse_index(const char *key, const char **out_end)
{
    if (!key) return -1;
    if (strncmp(key, "send", 4) != 0) return -1;
    const char *p = key + 4;
    if (*p < '1' || *p > '9') return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (out_end) *out_end = p;
    return n;
}

/*
 * Route a send key.
 *
 * "send1:fx3:cutoff" -> send 0, slot 2, param "cutoff"
 * "send1:return"     -> send 0, slot -1, param "return"   (a bus-level key)
 *
 * Returns 1 on a match, 0 otherwise. On 0 nothing is written.
 */
static inline int send_fx_route(const char *key, int send_count, int slot_count,
                                int *out_send, int *out_slot, const char **out_param)
{
    const char *end = NULL;
    int n = send_fx_parse_index(key, &end);
    if (n < 1 || n > send_count) return 0;
    if (!end || *end != ':') return 0;
    const char *rest = end + 1;

    const char *fx_end = NULL;
    int f = master_fx_parse_index(rest, &fx_end);
    if (f >= 1) {
        if (f > slot_count) return 0;          /* past the cap: reject, do not clamp */
        if (!fx_end || *fx_end != ':') return 0;
        if (out_send)  *out_send  = n - 1;
        if (out_slot)  *out_slot  = f - 1;
        if (out_param) *out_param = fx_end + 1;
        return 1;
    }

    if (*rest == '\0') return 0;               /* "send1:" names nothing */
    if (out_send)  *out_send  = n - 1;
    if (out_slot)  *out_slot  = -1;
    if (out_param) *out_param = rest;
    return 1;
}

#endif /* SEND_FX_KEY_H */
```

- [ ] **Step 5: Wrapper and Makefile target**

`tests/host/test_send_fx_key.sh` reads both caps out of `shadow_chain_mgmt.h`
the way `test_master_fx_slot_routing.sh` reads `MASTER_FX_SLOTS`. Makefile:

```make
SEND_BUSES := $(shell awk '/^\#define SEND_BUSES /{print $$3}' ../../src/host/shadow_chain_mgmt.h)

$(BUILD_DIR)/test_send_fx_key: test_send_fx_key.c ../../src/host/send_fx_key.h ../../src/host/master_fx_key.h ../../src/host/shadow_chain_mgmt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -DTEST_SEND_BUSES=$(SEND_BUSES) -DTEST_SEND_FX_SLOTS=$(MASTER_FX_SLOTS) $< -o $@ $(LDFLAGS)
```

- [ ] **Step 6: Add the storage**

In `shadow_chain_mgmt.h`:

```c
/* Send chains are master_fx_slot_t arrays so the Master FX hosting, bypass,
 * capture-rules and preset machinery is reused rather than duplicated. */
extern master_fx_slot_t shadow_send_fx_slots[SEND_BUSES][SEND_FX_SLOTS];
extern int shadow_send_return_level[SEND_BUSES];   /* 0..127 */
extern int shadow_send_a_to_b;                     /* 0..127 */
```

- [ ] **Step 7: Add the mix path**

In `src/schwung_shim.c`, near `shadow_slot_deferred` (around line 490):

```c
/* Global send accumulators. Shared by all four slots — that is what makes the
 * sends global: one reverb for the device rather than one per slot. Filled by
 * each slot's buses during render, drained here. */
static int16_t send_accum[SEND_BUSES][FRAMES_PER_BLOCK * 2];
static int16_t send_out[SEND_BUSES][FRAMES_PER_BLOCK * 2];
```

Clear them where `shadow_slot_deferred` is cleared (~line 1890). Then, in
`shim_post_transfer` immediately BEFORE the Master FX loop at ~line 2851:

```c
    /* Send buses. Each is hosted exactly like Master FX. */
    for (int sb = 0; sb < SEND_BUSES; sb++) {
        memcpy(send_out[sb], send_accum[sb], sizeof(send_out[sb]));

        /* A -> B: applied AFTER A's chain and BEFORE B's, which is what makes
         * it feedback-safe by construction — there is no point in the ordering
         * at which B's output can reach A. No loop detection is needed. */
        if (sb == 1 && shadow_send_a_to_b > 0) {
            int lvl = (shadow_send_a_to_b * shadow_send_return_level[0]) / 127;
            bus_mix_send(send_out[1], send_out[0], FRAMES_PER_BLOCK * 2, lvl);
        }

        for (int fx = 0; fx < SEND_FX_SLOTS; fx++) {
            master_fx_slot_t *s = &shadow_send_fx_slots[sb][fx];
            if (!(s->instance && s->api && s->api->process_block)) continue;
            int16_t dry[FRAMES_PER_BLOCK * 2];
            if (s->bypassed) memcpy(dry, send_out[sb], sizeof(dry));
            s->api->process_block(s->instance, send_out[sb], FRAMES_PER_BLOCK);
            if (s->bypassed) memcpy(send_out[sb], dry, sizeof(dry));
        }

        /* Returns sum into the mix BEFORE Master FX, so the master chain
         * processes the wet signal too. */
        bus_mix_send(fx_target, send_out[sb], FRAMES_PER_BLOCK * 2,
                     shadow_send_return_level[sb]);
    }
```

Add `#include "bus_mix.h"` and `#include "send_fx_key.h"` to `schwung_shim.c`.

Note the A->B block must run for `sb == 1` before B's own chain, which the loop
order above gives; if `SEND_BUSES` is ever raised past 2, this needs to become a
general routing matrix rather than a special case, and the `_Static_assert`
below guards that:

```c
_Static_assert(SEND_BUSES == 2, "the A->B special case assumes exactly two sends");
```

- [ ] **Step 8: Feed the accumulators from the chain**

Export from the chain host, called by the shim after each slot renders:

```c
/* Exported: drain this slot's per-bus send contributions into the caller's
 * accumulators. Called once per slot per frame, after render_block.
 *
 * POST-INSERT AND POST-FADER: the bus buffers already carry their inserts, and
 * the caller passes the slot's volume so pulling a track down pulls it out of
 * the sends, as a console does. */
void chain_drain_sends(void *instance, int16_t *const *accum, int n_sends,
                       int frames, int slot_volume_0_127);
```

- [ ] **Step 9: Run everything**

```bash
bash tests/host/test_send_fx_key.sh
make -C tests/host test
./scripts/build.sh
```
Expected: `PASS`, all green, build succeeds.

- [ ] **Step 10: Commit**

```bash
git add src/host/send_fx_key.h tests/host/test_send_fx_key.* tests/host/Makefile src/host/shadow_chain_mgmt.h src/host/shadow_chain_mgmt.c src/schwung_shim.c src/modules/chain/dsp/chain_host.c
git commit -m "shim: two global send buses, hosted like Master FX

Design credit PR #121 (legsmechanical). A->B is applied after A's chain and
before B's, so B can never reach A and no loop detection is needed. Returns
sum before Master FX so the master chain processes the wet signal."
```

---

## Task 7: Stems 6 and 7

**Goal:** The two send returns are captured as their own stems, restoring the exact-sum property that a shared return would otherwise break.

**Files:**
- Modify: `src/host/shadow_sampler.h` (`SAMPLER_STEM_COUNT` 5 -> 7, indices)
- Modify: `src/host/shadow_sampler.c` (`sampler_stem_names`)
- Modify: `src/schwung_shim.c` (`shadow_stem_dispatch`)

**Acceptance Criteria:**
- [ ] `SAMPLER_STEM_COUNT` is 7; `sampler_stem_names` gains `"SendA"`, `"SendB"` in index order
- [ ] The send stems capture the returns POST-send-chain and PRE-Master-FX, matching every other stem
- [ ] Stems 1-4 + 6 + 7 sum to the master bit-exactly with no Master FX loaded
- [ ] A send with no audio leaves no file (the existing delete-at-finalize rule covers it unchanged)
- [ ] The Skipback stem cap still applies to the new stems (60 s), so the ring budget does not grow unbounded

**Verify:** `make -C tests/host test` green; on device, record a take with a send loaded and confirm `sox` shows master == sum of stems

**Steps:**

- [ ] **Step 1: Widen the count**

In `shadow_sampler.h`:

```c
#define SAMPLER_STEM_COUNT 7
#define SAMPLER_STEM_MOVE  4   /* index of the Move stem; 0-3 are the slots */
#define SAMPLER_STEM_SEND_A 5
#define SAMPLER_STEM_SEND_B 6
```

Extend the comment block above it: the four slot stems plus the two send stems
sum to the master exactly; the Move stem remains the Move->Schwung-off case and
stays INVALID under Move->Schwung, or a stem sum would double every instrument.

In `shadow_sampler.c`:

```c
const char *const sampler_stem_names[SAMPLER_STEM_COUNT] = {
    "Slot1", "Slot2", "Slot3", "Slot4", "Move", "SendA", "SendB"
};
```

- [ ] **Step 2: Dispatch the new stems**

In `schwung_shim.c` at the `shadow_stem_dispatch(sampler_capture_stems)` call
(~line 3121), the pointer table gains the two `send_out[]` buffers. They must be
sampled at the same point as the slot stems — post-send-chain, pre-Master-FX —
which is where `send_out[]` sits after Task 6's loop.

- [ ] **Step 3: Verify the sum property**

Record a 4-bar take with one slot playing into Send A, no Master FX. Then:

```bash
# on the Mac, after pulling the files
sox -m -v 1 Take_Slot1.wav -v 1 Take_SendA.wav -t wav - | \
  sox -m - -v -1 Take.wav -n stat 2>&1 | grep "Maximum amplitude"
```
Expected: `Maximum amplitude: 0.000000` — the difference is silence.

- [ ] **Step 4: Commit**

```bash
git add src/host/shadow_sampler.h src/host/shadow_sampler.c src/schwung_shim.c
git commit -m "stems: capture the two send returns as stems 6 and 7

A shared send return belongs to no slot, so without these the four slot stems
no longer sum to the master. Captured post-send-chain and pre-Master-FX, like
every other stem."
```

---

## Task 8: Persistence and the param surface

**Goal:** Buses, voice assignments and send levels round-trip through the slot file; send chains, return levels and A->B round-trip per set; send presets are one store shared by A and B.

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (`bus<N>:` dispatch in `v2_set_param`/`v2_get_param`)
- Modify: `src/modules/chain/dsp/chain_patch.c` (bus serialization)
- Modify: `src/host/shadow_chain_mgmt.c` (`send_fx_N.json`, shared preset store)

**Acceptance Criteria:**
- [ ] `bus<N>:create`, `:delete`, `:name`, `:voices`, `:send<M>`, `:fx<K>:module`, `:fx<K>:<param>`, `:fx<K>:bypassed` all route through `bus_route.h`
- [ ] A bus whose stored voice ids no longer exist keeps its chain and REPORTS the orphaned ids; it does not silently re-point to whatever is at that index now
- [ ] Restored state writes STATE, never SHAPE — restoring a bus does not reinstantiate a running FX whose module is unchanged
- [ ] The shim is authoritative for send chains: `send<N>:modules` is one GET returning the whole chain, positional, never compacted
- [ ] A send preset saved from A loads onto B
- [ ] Autosave's existing bail-if-empty and skip-if-unchanged guards cover buses without new copies of them

**Verify:** `./scripts/build.sh`; on device, build a two-bus kit, reboot, confirm it comes back; save a preset on A and load it on B

**Steps:**

- [ ] **Step 1: Add the `bus<N>:` dispatch**

At the top of `v2_set_param` in `chain_host.c`, before the existing `synth:` and
`fx<N>:` ladders:

```c
    {
        int b = -1;
        const char *rest = NULL;
        if (bus_route_param_key(key, SLOT_BUSES, &b, &rest)) {
            return chain_bus_set_param(inst, b, rest, val);
        }
    }
```

and the mirror in `v2_get_param`. Implement `chain_bus_set_param` /
`chain_bus_get_param` in a new `src/modules/chain/dsp/chain_bus.c` — a sibling
file rather than more of `chain_host.c`, which is already 2206 lines and was
split once for exactly this reason.

- [ ] **Step 2: Serialize**

In `chain_patch.c`, beside the existing slot serialization, write and read:

```json
"buses": [
  {"name": "Kick", "voices": ["kick"], "sends": [20, 0],
   "fx": [{"module": "tapescam", "state": "...", "bypassed": 0}]},
  {"name": "Hats", "voices": ["chh", "ohh"], "sends": [0, 15],
   "fx": [{"module": "chorus", "state": "..."}, {"module": "phaser", "state": "..."}]}
],
"main_sends": [5, 30]
```

Voices are stored as **ids**. On load, resolve each through `bus_voice_index`;
an id that misses is retained in the config and counted:

```c
/* An orphaned id is REPORTED, not dropped and not re-pointed. A partial
 * restore that reports nothing is indistinguishable from a working one —
 * the same rule the snapshot/recall count exists for. */
if (idx < 0) inst->buses[b].orphan_count++;
else inst->voice_bus[idx] = (int8_t)b;
```

- [ ] **Step 3: Send persistence, shim-authoritative**

In `shadow_chain_mgmt.c`, mirror the `master_fx:modules` handling:

```c
/* ONE GET returning the whole chain, positional and never compacted.
 *
 * THE SHIM SAYS WHAT IS LOADED. The Master FX in-file mirror never saw
 * anything written straight to the shim — an overtake tool, a Remote UI
 * client — and wrote {} over it, losing the whole chain on the next boot.
 * Same rule here. */
if (strcmp(param, "modules") == 0) { /* serialize all SEND_FX_SLOTS positions */ }
```

Write `send_fx_N.json` beside `master_fx_N.json`, per set, holding both chains,
both return levels and the A->B amount.

- [ ] **Step 4: Shared preset store**

Send presets live in one directory, not one per bus, so a preset saved from A
loads onto B. Reuse the Master FX preset code path with the store directory as
a parameter rather than copying it.

- [ ] **Step 5: Build and round-trip on device**

```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```
Then: build a kit with two buses and a send, reboot, confirm it returns; save a
preset from Send A and load it onto Send B.

- [ ] **Step 6: Commit**

```bash
git add src/modules/chain/dsp/chain_bus.c src/modules/chain/dsp/chain_host.c src/modules/chain/dsp/chain_patch.c src/host/shadow_chain_mgmt.c
git commit -m "buses: persist voice sets by id, sends per set from the shim

Voice ids, not indices, so a module adding a voice does not re-point every
bus; an id that no longer resolves is reported rather than re-pointed. Send
chains are read from the shim in one positional GET, as Master FX is."
```

---

## Task 9: The FX-bus picker

**Goal:** Shift+Vol+Menu opens a picker listing Master FX, Send A and Send B; Master FX becomes one bus among them.

**Files:**
- Modify: `src/shadow/shadow_ui.js` (the Menu / Shift+Vol+Menu entry, a `VIEWS.FX_BUS_PICKER`)
- Modify: `src/shadow/shadow_ui_master_fx.mjs` (parameterize by bus)

**Acceptance Criteria:**
- [ ] Shift+Vol+Menu and hold-Menu both open the picker rather than Master FX directly
- [ ] Choosing Master FX gives the screen that exists today, unchanged
- [ ] Choosing Send A or Send B gives the same 8-position editor against that send's chain
- [ ] A send's editor shows its return level, and Send A's shows the -> Send B amount
- [ ] Sends show no LFO rows; Master keeps them
- [ ] Back from a bus returns to the picker; Back from the picker dismisses, matching every other list

**Verify:** `bash tests/host/test_master_fx_slot_routing.sh` and the shadow suite stay green; render the picker and both editors through the PNG harness and LOOK at them

**Steps:**

- [ ] **Step 1: Add the view and the list**

The picker is a list of three rows. Use the one list engine — do not hand-roll
rows. Every scrolling list draws a scrollbar and no list draws arrows.

- [ ] **Step 2: Parameterize the Master FX editor by bus**

`shadow_ui_master_fx.mjs` currently addresses `master_fx:` keys directly. Give
it a bus descriptor `{keyPrefix, hasLfos, extraRows}` so `master_fx:` /
`send1:` / `send2:` all drive the same code.

- [ ] **Step 3: Render and look**

```bash
mkdir -p /tmp/schwung-png
DUMP_PNG=/tmp/schwung-png bash tests/host/test_chain_editor_snapshot.sh
open /tmp/schwung-png
```

That test renders the chain-editor cases and, with `DUMP_PNG` set, writes each
as a 5x PNG (`DUMP_CASE=picker/` narrows it). Add cases for the picker and both
send editors to its case list, then LOOK at the images — seven defects have sat
in these renders before. In particular `drawFooter` DROPS a hint pair that does
not fit, silently, along with every pair after it, so check the footer actually
names the verb of the row under the cursor.

- [ ] **Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js src/shadow/shadow_ui_master_fx.mjs
git commit -m "shadow_ui: an FX-bus picker, with Master FX as one of the buses

Design credit PR #121 (legsmechanical)."
```

---

## Task 10: The bus list and the down-gesture

**Goal:** Down on the synth box opens the slot's bus list; down on a bus row opens that bus's 8-position chain in the ordinary editor.

**Files:**
- Create: `src/shadow/shadow_ui_buses.mjs`
- Modify: `src/shadow/shadow_ui.js` (gesture, view, wiring)

**Acceptance Criteria:**
- [ ] Down on the synth box in `VIEWS.CHAIN_EDIT` opens the bus list; down elsewhere in that view is unchanged
- [ ] A slot whose synth publishes no `split_voices` shows NO bus affordance — no row, no hint, no empty screen
- [ ] The list shows Main plus each bus, with its insert summary and both send levels
- [ ] Create / rename / delete / assign-voices are reachable, and the footer names the verb of the row under the CURSOR
- [ ] Voice assignment is a multi-select over `split_voices` labels, showing which bus each voice is currently on
- [ ] Down on a bus row opens the existing chain editor against `bus<N>:`
- [ ] The path writes NO pad LEDs — Move owns the pads while the shadow UI is up
- [ ] A `null` read of `split_voices` shows a waiting state and retries; it never empties the list or latches

**Verify:** render every screen through the PNG harness and look; `for t in tests/host/*.sh; do bash "$t"; done` green

**Steps:**

- [ ] **Step 1: Write the module**

`src/shadow/shadow_ui_buses.mjs` exports the list model, the voice-assignment
model, and a render taking `(ctx, {rect, bands})` — nothing in a param-pages
module clears the screen, which is what lets a page sit inside a caller's
chrome. Anything full-screen is the frame owner's second call.

- [ ] **Step 2: Wire the gesture**

In `shadow_ui.js`'s `VIEWS.CHAIN_EDIT` input handler: down, when the cursor is
on the synth box AND `splitVoices?.length`, enters the bus list.

Remember the dispatch order: whatever is drawn LAST must be fed FIRST. The draw
path is a switch with overlays painted after it; the input path is a run of
early-outs before it, and the two orders are the reverse of each other.

- [ ] **Step 3: Guard the tri-state read**

```js
// A timed-out read empties NOTHING and latches NOTHING. `null` is not news
// about the module — branch on the RAW value before parsing, because
// parse(null) and parse("") both give null and by then the distinction is gone.
const raw = shadow_get_param(`slot${slot}:synth:split_voices`);
if (raw === null) { contractUnresolved = true; return; }   // retry, keep prior
const voices = raw === "" ? [] : JSON.parse(raw);
```

- [ ] **Step 4: Render and look**

Add cases to `tests/host/test_chain_editor_snapshot.sh` and dump them:

```bash
mkdir -p /tmp/schwung-png
DUMP_PNG=/tmp/schwung-png bash tests/host/test_chain_editor_snapshot.sh
open /tmp/schwung-png
```

Cases to add and inspect: the bus list with 1, 3 and 6 rows; the voice
assignment screen with 16 voices; a bus chain with 0, 1 and 8 positions; and a
slot whose module cannot split, which must render NO bus affordance at all.

- [ ] **Step 5: Commit**

```bash
git add src/shadow/shadow_ui_buses.mjs src/shadow/shadow_ui.js
git commit -m "shadow_ui: buses hang below the synth box, one jog down

A sibling .mjs rather than more of shadow_ui.js, which is already ~14k lines."
```

---

## Task 11: Send levels on the knob grid

**Goal:** Send levels are rideable on the encoders, not only settable in a list.

**Files:**
- Modify: `src/shadow/shadow_ui_buses.mjs` (a synthesised contract for the bus mixer page)
- Modify: `src/shadow/shadow_ui_param_pages.mjs` (page registration)

**Acceptance Criteria:**
- [ ] A "Bus Sends" page exposes each bus's A and B level on an encoder
- [ ] The page is handed `paginate: false` if it is one authored grouping, per the Global Settings rule — one section, one page, however long
- [ ] Values arrive on touch-down / on the rotation / in the entry warm, NEVER on the draw path (an IPC read is ~2.8 ms; a whole page render is 1.68 ms)
- [ ] A read that did not answer draws no picture — no placeholder frame

**Verify:** render the page; confirm through the read-budget test (`tests/host/test_chain_edit_read_budget.sh`) that the draw path adds no reads

**Steps:**

- [ ] **Step 1: Synthesise the contract**

Follow Global Settings / Slot Settings: a `chain_params`-shaped array built in
JS, with `type: "int"`, `min: 0`, `max: 127` per level.

- [ ] **Step 2: Render the page and look**

```bash
node tools/param-pages/preview.mjs
```

- [ ] **Step 3: Verify the read budget**

```bash
bash tests/host/test_chain_edit_read_budget.sh
```
Expected: green. If it fails, a read has landed on the draw path.

- [ ] **Step 4: Commit**

```bash
git add src/shadow/shadow_ui_buses.mjs src/shadow/shadow_ui_param_pages.mjs
git commit -m "grid: a Bus Sends page, so send levels can be ridden"
```

---

## Task 12: Documentation and credit

**Goal:** Every doc that describes the chain, the module contract or the shortcuts reflects buses and sends, and PR #121 is credited in public.

**Files:**
- Modify: `CLAUDE.md` (one bullet per subsystem hook — NOT the prose)
- Modify: `docs/CHAIN.md` (the module contract: `split_voices`, `render_split`)
- Modify: `docs/MODULES.md` (how a module author opts in)
- Modify: `src/host/plugin_api_v1.h` (the `render_split` contract beside the threading one)
- Modify: `docs/SHADOW_UI.md` (the picker, the bus list, send persistence)
- Modify: `src/shared/help_content.json`
- Modify: `../schwung-catalog-site/manual.html` (buses, sends, the new gestures)

**Acceptance Criteria:**
- [ ] `CLAUDE.md` gains a bullet under the CHAIN and SHADOW_UI hooks, not paragraphs — it is an INDEX, and adding prose inline is how it reached 151 KB
- [ ] The `render_split` accumulate-and-alias contract appears in `plugin_api_v1.h`, `docs/MODULES.md` and `docs/CHAIN.md`, and the three are consistent
- [ ] PR #121 and legsmechanical are credited in `docs/CHAIN.md` and in the commit trailer
- [ ] `manual.html` documents the down-gesture and the FX-bus picker (a gesture changed, so this is required, not optional)
- [ ] `bash tests/host/test_builtin_help_content.sh` green

**Verify:** `for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done` -> no failures

**Steps:**

- [ ] **Step 1: Write the `CLAUDE.md` bullets**

Under the `docs/CHAIN.md` hook:

```markdown
- **A module declares which voices it can render APART, and the grouping is
  ours.** `split_voices` is a FLAT ORDERED list whose index is the buffer
  index — the map resolves in C on the callback and `chain_json.c` cannot walk
  `levels` in order, the same constraint behind `synth:last_note`. Render is a
  **dlsym'd `move_plugin_render_split`, never a field on `plugin_api_v2_t`**
  (breakbeat's header drift boot-looped a device). It **ACCUMULATES**, and its
  `voice_out[]` entries **ALIAS**: two voices in one bus get one pointer, so
  the summing is free and the sparse case costs nothing.
```

Under the `docs/SHADOW_UI.md` hook:

```markdown
- **Sends are GLOBAL, and that is a cost decision.** Per-slot sends mean four
  reverbs when four slots want one. Two device-wide buses, hosted as
  `master_fx_slot_t` like Master FX, post-insert and post-fader, with A->B
  applied between A's chain and B's so B can never reach A. A send return
  belongs to no slot, so it lands in **stems 6 and 7** — without them the four
  slot stems stop summing to the master. Design credit: PR #121.
```

- [ ] **Step 2: Update the manual**

```bash
node tools/param-pages/widget_sheet.mjs --manual
```
only if a widget changed; otherwise edit `manual.html` by hand. Note the sibling
repo's `manual.html` is dirty with an unmerged Boot menu section — **commit
hunks, never the file.**

- [ ] **Step 3: Run the full suite**

```bash
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
./scripts/build.sh
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/ src/host/plugin_api_v1.h src/shared/help_content.json
git commit -m "docs: module buses and global sends

Send topology, the post-fader rule, return levels, A->B and shared presets are
PR #121's design (legsmechanical), re-implemented on current main because that
branch's merge-base is 2026-03-04.

Co-Authored-By: legsmechanical <noreply@github.com>"
```

---

## Verification

```bash
make -C tests/host test                                   # CI gate 1
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
./scripts/build.sh                                        # CI gate 3 (ARM64 Docker)
./scripts/install.sh local --skip-modules --skip-confirmation
```

Then on hardware, with a splittable module in a slot:

1. Down on the synth box -> bus list appears
2. Create a bus, assign two voices, load two FX -> only those voices are affected
3. Raise the bus's Send A -> the shared effect is heard, once, not four times
4. Turn the slot volume down -> the send follows it (post-fader)
5. Set Send A -> B -> A's output washes through B; B does not feed back into A
6. Save a preset on Send A, load it on Send B
7. Reboot -> the kit, the buses and the sends all come back
8. Record a take -> master == stems 1-4 + 6 + 7, bit-exact, with no Master FX
9. `/system/cpu` -> the added cost is attributed and visible

## Explicitly out of scope

- **LFO targeting of bus FX.** `bus_route_target` exists and is tested in Task 2
  because the key format has to be decided once, but no task wires a slot LFO to
  a `bus<N>` target. Sends have no LFOs either (Task 9). Adding both later is a
  self-contained follow-up needing no change to anything above.
- **#121's Move FX half** — four per-Move-track insert buses with a
  `Move>SchwFX` peel. A separate feature about Move's own tracks; folding it in
  is what made #121 too large to land.
- **Any CPU throttle or dynamic cap.** The ceiling was chosen deliberately; the
  mitigation is visibility on `/system/cpu`, not enforcement.

## Open risks

- **CPU.** A slot can now hold 40 positions against a ~2370 us frame in which a
  Tape-Echo-class effect measures 0.37 ms. Accepted deliberately; the mitigation
  is visibility on `/system/cpu`, not a throttle.
- **Only one module will implement `render_split` at first.** Everything else
  falls back to `render_block` into Main and offers no buses. The fallback is
  the tested default, not an error path.
- **PR #121 is still open.** Landing this closes it as superseded; that
  conversation is the user's to have, not the implementer's.
