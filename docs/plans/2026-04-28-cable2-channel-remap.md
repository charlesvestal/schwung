# Cable-2 MIDI Channel Remap (Shim Feature)

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let any overtake module remap incoming external (cable-2) MIDI channels before Move's firmware processes them. Solves SEQ8's "live monitoring through arbitrary Move tracks" problem without re-injection (which causes the cable-2 echo cascade documented in `~/schwung-docs/MIDI_INJECTION.md` and `MEMORY.md → midi_fx_to_move_status`).

**Why a shim feature, not JS:** the natural JS-side fix — receive cable-2 in `onMidiMessageExternal`, re-inject with the desired channel — is broken. Move echoes cable-2 IN back on cable-2 OUT, which fires `onMidiMessageExternal` again, which re-injects, which echoes, which fires again. Cascade fills the 64-packet inject ring within milliseconds and crashes. Refcount mitigation fails because MIDI_OUT only has 20 slots per frame and cable-0 events (pads/LEDs) starve cable-2 echoes, leaking the refcount. The only clean fix is to rewrite the channel byte in the hardware mailbox before Move sees it.

**Tech Stack:** C in `schwung_shim.c`, LD_PRELOAD, runs in pre-ioctl SPI callback context. One new SHM segment. No allocations, no locks, no logging in the callback path.

**Risk:** Low. The remap is a single-byte rewrite on existing buffer reads the shim already does. Audio path is untouched. Worst case is wrong channels reach Move (audible immediately, easy to bisect). Recommend a worktree because it touches the SPI hot path, but no full test matrix needed.

**Recommendation:** Build in `../schwung-cable2-remap` worktree.

---

## Context: What's There Today

**MIDI_IN buffer the shim sees:** 256 bytes at `MIDI_IN_OFFSET`, 64 USB-MIDI packets × 4 bytes each. Cable nibble is `(byte[0] >> 4) & 0x0F`, status at `byte[1]`. (Note: the kernel-side raw SPI uses 8-byte events with timestamps, but the SPI library presents stride-4 USB-MIDI to the shim. All existing shim code already iterates stride 4 — see `schwung_shim.c:1193`, `:5161`, `:5276`, `:3806`.)

**Existing readers of cable-2 MIDI_IN that run before any remap could land:**
- `shadow_dispatch_direct_external_midi()` — `schwung_shim.c:4303`. Direct-dispatches all message types from cable-2 to MPE passthrough slots (receive=All, forward=THRU). Reads original channel.
- `shadow_forward_external_cc_to_out()` — `schwung_shim.c:4296`. Copies CC/PB/AT from cable-2 IN to MIDI_OUT so DSP routing can pick them up.
- Cable-2 echo filter — `schwung_shim.c:1192-1201`. Scans MIDI_IN to decide if MIDI_OUT cable-2 events are echoes.

**Existing post-ioctl filter:** `schwung_shim.c:5161` mutates only the *shadow copy* (`sh_midi`), not the hw mailbox. Insufficient for our purposes — Move reads from the hw mailbox directly.

**Cable-2 echoes (the cascade trigger):** confirmed only for note messages; CC/PB/AT do not echo (per comment at `schwung_shim.c:4293-4294`). System messages (status `0xF*`) are channelless.

**MPE passthrough slots:** when forward=THRU is configured on a slot, that slot relies on per-MIDI-channel expression data. Remapping channels destroys this. We disable remap globally whenever any THRU slot is active.

---

## SHM Contract

New segment: `/schwung-ext-midi-remap`, 64 bytes total.

```c
/* In src/host/shadow_constants.h */
#define SHM_SHADOW_EXT_MIDI_REMAP "/schwung-ext-midi-remap"

typedef struct {
    uint8_t version;          /* 1 = current */
    uint8_t enabled;          /* 0 = bypass entire feature, 1 = active */
    uint8_t remap[16];        /* remap[in_ch] = out_ch (both 0-indexed).
                               * 0xFF = passthrough (no remap for that channel).
                               * Writer: active overtake module via JS.
                               * Reader: shim, every SPI frame. */
    uint8_t _reserved[46];    /* reserved for v2 (per-source remap, velocity scale, note filters) */
} schwung_ext_midi_remap_t;   /* 64 bytes */
```

**Ownership convention:**
- Created by shim on startup (zero-init, all `0xFF` in remap table, `enabled=0`).
- Written only by the active overtake module (JS via host helper, see Task 5).
- Reset to all-`0xFF` and `enabled=0` by shim on overtake exit (forced — never trusted to JS, since shadow_ui.js cleanup doesn't always run cleanly on crash).

---

## Tasks

### Task 1: SHM segment + struct definitions

**File:** `src/host/shadow_constants.h`

Add the `schwung_ext_midi_remap_t` struct and `SHM_SHADOW_EXT_MIDI_REMAP` define alongside the existing midi_inject definitions (around line 30, 216-223).

**File:** `src/schwung_shim.c`

Add file-scope statics next to the existing `shadow_midi_inject_shm` block (~line 2300):

```c
static schwung_ext_midi_remap_t *ext_midi_remap_shm = NULL;
static int shm_ext_midi_remap_fd = -1;
```

Open / mmap / zero-init in `shadow_shm_init()` (~line 2750), mirroring the existing midi_inject pattern:

```c
shm_ext_midi_remap_fd = shm_open(SHM_SHADOW_EXT_MIDI_REMAP, O_CREAT | O_RDWR, 0666);
if (shm_ext_midi_remap_fd >= 0) {
    ftruncate(shm_ext_midi_remap_fd, sizeof(schwung_ext_midi_remap_t));
    ext_midi_remap_shm = mmap(NULL, sizeof(schwung_ext_midi_remap_t),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_ext_midi_remap_fd, 0);
    if (ext_midi_remap_shm == MAP_FAILED) {
        ext_midi_remap_shm = NULL;
    } else {
        memset(ext_midi_remap_shm, 0xFF, sizeof(schwung_ext_midi_remap_t));
        ext_midi_remap_shm->version = 1;
        ext_midi_remap_shm->enabled = 0;
        memset(ext_midi_remap_shm->_reserved, 0, sizeof(ext_midi_remap_shm->_reserved));
    }
}
```

**Verification:** build, deploy, ssh in, `ls -la /dev/shm/schwung-ext-midi-remap` shows 64 bytes after launch.

### Task 2: MPE-active gate

The remap is suppressed whenever any chain slot is configured forward=THRU (`SHADOW_FORWARD_THRU = -2`).

**File:** `src/schwung_shim.c`

Add a helper near `shadow_chain_slots[]`:

```c
static int any_thru_slot_active(void) {
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        if (shadow_chain_slots[i].forward_channel == SHADOW_FORWARD_THRU) {
            return 1;
        }
    }
    return 0;
}
```

Confirm the constant name and slot field name by grepping — adjust to match what's actually in the codebase.

### Task 3: The remap itself (pre-ioctl, on hw mailbox)

**File:** `src/schwung_shim.c`

Add a function and call it from the pre-transfer callback **before** any of the existing readers (`shadow_dispatch_direct_external_midi`, `shadow_forward_external_cc_to_out`, the cable-2 echo filter at line 1192). The earliest safe insertion point is right after the SPI library populates the hardware MIDI_IN buffer and before any shim helper inspects it.

```c
static void shim_remap_cable2_channels(void) {
    if (!ext_midi_remap_shm || !ext_midi_remap_shm->enabled) return;
    if (any_thru_slot_active()) return;       /* MPE escape hatch */
    if (!hardware_mmap_addr) return;

    uint8_t *buf = hardware_mmap_addr + MIDI_IN_OFFSET;
    for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
        uint8_t header = buf[j];
        if (header == 0) continue;            /* empty slot */
        uint8_t cable = (header >> 4) & 0x0F;
        if (cable != 2) continue;             /* only cable-2 (external USB) */
        uint8_t status = buf[j + 1];
        if ((status & 0xF0) == 0xF0) continue; /* system messages — channelless */
        uint8_t in_ch = status & 0x0F;
        uint8_t mapped = ext_midi_remap_shm->remap[in_ch];
        if (mapped == 0xFF || mapped >= 16) continue; /* passthrough */
        buf[j + 1] = (status & 0xF0) | (mapped & 0x0F);
    }
}
```

**Insertion point:** call from `shim_pre_transfer()` (find the function around `schwung_shim.c:4280`, before `shadow_drain_web_param_set`). Confirm the exact pre-transfer entry by checking where the SPI library invokes the pre callback.

**Critical:** the remap MUST run before line 4296 (`shadow_forward_external_cc_to_out`) and line 4303 (`shadow_dispatch_direct_external_midi`). Both read cable-2 from MIDI_IN, and we want them to see remapped channels too — otherwise JS modules and Move firmware disagree.

### Task 4: Forced reset on overtake exit

**File:** `src/schwung_shim.c`

In `shim_post_transfer()`, in the existing overtake-exit-detection block (around line 5063, where shift-off / volume-touch-off are injected):

```c
if (prev_overtake_mode != 0 && overtake_mode == 0 && ext_midi_remap_shm) {
    memset(ext_midi_remap_shm->remap, 0xFF, 16);
    ext_midi_remap_shm->enabled = 0;
}
```

This makes the cleanup unconditional regardless of whether the JS module reset its table. Stale tables from crashed modules can't bleed into the next session.

### Task 5: JS host bindings

**File:** `src/schwung_host.c` (or wherever the QuickJS host bindings live — grep for existing `js_host_*` functions and follow the pattern).

Add three functions exposed to JS:

```c
/* Set remap for one channel. in_ch and out_ch are 0-indexed.
 * out_ch = 0xFF or any value > 15 means passthrough. */
static JSValue js_host_ext_midi_remap_set(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv);

/* Clear all remaps (full passthrough). */
static JSValue js_host_ext_midi_remap_clear(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);

/* Enable/disable the entire feature. */
static JSValue js_host_ext_midi_remap_enable(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv);
```

These must mmap the same SHM segment (the host process is separate from the shim). Wrap mmap in a one-shot init (open if not yet open, then write). Mirror how `host_module_send_midi` accesses `shadow_midi_inject_shm`.

Update `docs/API.md` with the new host functions:

```
host_ext_midi_remap_set(in_ch, out_ch)  // 0-15, or out_ch >= 16 for passthrough
host_ext_midi_remap_clear()
host_ext_midi_remap_enable(on)          // bool
```

### Task 6: Feature flag

**File:** `features.json` (and the corresponding default in source — grep for `link_audio_enabled` to find the pattern)

Add `"ext_midi_remap_enabled": true` (default on; the SHM gate (`enabled` byte) is the per-session control, but the feature flag lets us kill the entire codepath if it misbehaves).

In `shim_remap_cable2_channels()`, gate on the feature flag too:

```c
if (!feature_ext_midi_remap_enabled) return;
```

### Task 7: Documentation

- `CLAUDE.md` — add a "Cable-2 Channel Remap" subsection under "MIDI Cable Filtering" describing the shim feature, the SHM contract, and the MPE escape hatch.
- `docs/API.md` — document the three new `host_ext_midi_remap_*` functions.
- `docs/MIDI_INJECTION.md` — add a closing note: "as of v0.X.Y, channel remap is now handled in the shim via `/schwung-ext-midi-remap`; do not re-inject from `onMidiMessageExternal`."

---

## Out of Scope (Reserved for v2)

The 46 reserved bytes in the SHM struct are intentional. **Do not** add any of these in v1:
- Per-source remap (e.g., different table per cable, or per device id)
- Velocity scaling / curves
- Note range filters
- CC remap / curves
- MPE-aware split mode

If a second use case lands, design the v2 contract then. Speculative APIs rot.

---

## Verification

**Manual on-device, with SEQ8:**
1. Plug a USB-A MIDI keyboard into Move set to transmit on ch1.
2. Configure Move tracks: T1 In=ch1, T2 In=ch2, T3 In=ch3, T4 In=ch4.
3. Load SEQ8, open shadow UI.
4. With remap inactive: keyboard plays only T1 (Move's auto-routing). ✓
5. SEQ8's Ch knob → ch2: from JS, call `host_ext_midi_remap_set(0, 1)` and `host_ext_midi_remap_enable(true)`. Keyboard now plays only T2. ✓
6. SEQ8's Ch knob → ch3, ch4: same pattern. Each channel correctly retargets. ✓
7. SEQ8's Ch knob → "Original": call `host_ext_midi_remap_clear()`. Keyboard plays T1 again. ✓
8. Exit overtake (Shift+Vol+Jog Click): plug keyboard back in, verify keyboard still plays T1 (forced reset worked). ✓
9. Load a chain with a forward=THRU slot: verify remap is bypassed and MPE expression survives. ✓
10. Stress test: crash the overtake module mid-session (kill -9 on shadow_ui), verify remap table is reset on next overtake load. ✓

**Cascade regression check:** with remap active, hold a chord on the keyboard for 30+ seconds. No crash, no audible glitch, no growing latency. (This was the failure mode of the JS-side approach.)

---

## Notes for Implementer

- The ordering constraint in Task 3 is the single non-obvious thing in this plan. If the remap runs after `shadow_dispatch_direct_external_midi`, MPE passthrough slots (when present) and Move firmware will see different channels — *worse* than no remap. Verify call ordering with a temporary `LOG_DEBUG` before removing the log.
- `MIDI_BUFFER_SIZE` is 256 (64 packets × 4 bytes) — use the constant, not a literal.
- The `version` byte in the SHM is for forward compat — readers should reject unknown versions. Not implementing version checks now (only one version exists), but writers must always set it.
- One existing thing worth confirming during implementation: whether `hardware_mmap_addr` is non-NULL during `shim_pre_transfer`. If it's only valid post-transfer, we may need to use `global_mmap_addr` (the shadow buffer) and rely on the SPI library's pre-transfer copy. Grep for `hardware_mmap_addr` usage in pre-transfer to confirm.
