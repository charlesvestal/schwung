# Print Stems Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

> **RESUME POINT (pinned 2026-05-29):** Phases 0/1/2 complete; Phase 3 Tasks 3.1–3.3 complete and hardware-verified. **Next: Task 3.4** (single-pass capture state machine). All work committed on branch `print-stems` (latest `58435190`). Working tree's only uncommitted items (`.serena/project.yml`, `docs/plans/2026-05-27-chordism-*.md`) are pre-existing and unrelated.
> - **Done:** SHM ring + capture writer (both non-LA and LA-rebuild paths) + JS API `host_print_capture_read`/`_write_index`; 25 Python tests; tool module loads with grid parse (`abl_io.mjs:parseClipGrid`) + pad-firing primitives (`fireColumn`/`stopAllTracks`/inject queue with deferred note-offs).
> - **Open spikes:** 0.4 (set-browser refresh) and 0.5 (sampleUri subdirs) — needed before Phase 6 output assembly.
> - **Deviation:** the Song.abl *parser* was pulled forward from Phase 6 into `abl_io.mjs`; Phase 6 Task 6.1 now *adds* `buildStemsSongAbl`+`sampleUri` to that file instead of porting from scratch.
> - **Resume detail:** Task 3.4 should anchor capture-window lengths to `host_print_capture_write_index()` block counts (344/s), use wall-clock `barDurationMs` only for fire timing (~1 beat before a boundary), and repurpose the current jog-click `fireColumn(0)` manual test into the orchestrator. See memory `project_print_stems_status`.

**Goal:** Bounce every populated clip in the active Move set to its own per-track stereo WAV, packaged as a sibling Move audio-clip set, so Schwung-driven arrangements can move into Move audio sets or Live without DAW glue.

**Architecture:** Parallel per-track capture from pre-MFX buses (`shadow_slot_fx_deferred[t]` for Schwung slots — post-slot-FX, gated by valid flag, per Task 0.1; per-track Link Audio for Move-native tracks). One pass per column with adaptive inter-pass tail-clear. Output is a sibling `<setname> Stems` set whose audio clips reference flat WAV files in `UserLibrary/Recordings/`.

**Tech Stack:** C (shim, host, SHM), JavaScript (.js for UI module, .mjs for shared utils), QuickJS host functions, Move's `Song.abl` JSON schema (1.8.3). All testing is manual on hardware except Phase 2 (host-side parser/generator unit-tested with Python).

**Design doc:** `docs/plans/2026-05-29-print-stems-design.md` — read first.

**Key references to read before starting:**
- `src/modules/tools/song-mode/ui.js` — clip-grid parsing, pad firing, set discovery, persistence pattern
- `src/modules/tools/song-mode/module.json` — tool-module shape
- `src/schwung_shim.c` (lines ~1640–1660, ~2400–2600) — `shadow_slot_deferred`, `unity_view`, capture call sites
- `src/host/shadow_sampler.[ch]` — WAV writer + background thread + ring buffer pattern
- `src/host/shadow_constants.h` — SHM struct conventions
- `src/host/link_audio.h`, `src/host/shadow_link_audio.c` — Link Audio SHM reader
- `CLAUDE.md` — deploy command, device constraints, realtime safety rules

**Deploy command (memorize):**
```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Log tail:**
```bash
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
```

**Enable logger first time:**
```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
```

---

## Phase 0 — Spikes (answer riskiest open questions before building)

Goal: resolve open questions #1, #4, #5, #6, #7 from design doc before committing implementation details. Spikes are scratch experiments, not production code; their output is **decisions written into this plan**, not code that ships.

### Task 0.1: Verify shadow_slot_deferred is post–slot-FX

**Files to read:**
- `src/modules/chain/dsp/chain_host.c` — chain processing path, slot FX call order
- `src/schwung_shim.c` lines 1640–1660 — where `shadow_slot_deferred` is filled
- `src/host/shadow_chain_mgmt.c` — chain orchestration

**Step 1:** Read the three files and trace: does the data in `shadow_slot_deferred[t]` represent (a) the slot synth's raw output, (b) the slot output after the slot's chain audio-FX, or (c) the chain output summed with MFX wet?

**Step 2:** Write one paragraph in this plan (under this task) documenting the answer. If (b), proceed with the design as written. If (a), identify the buffer downstream of slot-FX in `chain_host.c` and use it instead. If (c), find/build a pre-MFX tap.

**Step 3:** Commit the plan update.
```bash
git add docs/plans/2026-05-29-print-stems.md
git commit -m "docs(print-stems): record slot-FX bus position from spike 0.1"
```

**Outcome (filled 2026-05-28):** **Answer is (a): `shadow_slot_deferred[t]` is the slot synth's raw, pre-FX output, NOT post-slot-FX.** Tracing the same-frame-FX path in `schwung_shim.c`: the shim calls `shadow_plugin_v2->render_block(..., shadow_slot_deferred[s], ...)` (line ~1601), and in `chain_host.c:v2_render_block` (around line 8871) `external_fx_mode` causes an early return *before* the audio-FX loop runs, so what lands in `shadow_slot_deferred[s]` is just the sound generator's raw render. Slot-FX is then applied separately later in the same frame: the shim memcpys `shadow_slot_deferred[s]` into a scratch buffer, calls `shadow_chain_process_fx(...)` on it (which loops over `inst->fx_plugins[*]->process_block` — same FX loop as `v2_render_block` would have run; `chain_host.c:chain_process_fx` around line 8961), and writes the result into `shadow_slot_fx_deferred[s]` (shim line ~1708). Confirmation that the latter is what reaches the master bus: the mixdown path at `schwung_shim.c:~2239` consumes `shadow_slot_fx_deferred[s]` (post-FX) when valid and only falls back to `shadow_slot_deferred[s]` to run FX inline. Caveat for the rebuild_from_la (Link Audio) path: `shadow_slot_fx_deferred[s]` is forced to zero (shim ~1690) because FX is re-run inside the Link Audio rebuild via `shadow_chain_process_fx`, so the Print Stems capture path must also have a fallback for that mode — either tap inside the LA rebuild loop or document Link Audio routing as out-of-scope for v1. **Design change required:** the SHM ring writes in Task 1.2 must source from `shadow_slot_fx_deferred[s]` (with `shadow_slot_fx_deferred_valid[s]` gating) rather than `shadow_slot_deferred[s]`. Update all subsequent references in this plan accordingly when implementing Phase 1.

---

### Task 0.2: Verify clip launch quantization via JS pad injection

**Files to read:**
- `docs/ADDRESSING_MOVE_SYNTHS.md` — how JS sends MIDI to Move tracks
- `src/modules/tools/song-mode/ui.js` lines around `move_midi_inject_to_move` calls — how it fires pads

**Step 1:** Write a 30-line throwaway tool module `src/modules/tools/quant-test/{module.json, ui.js}` that, on jog-click, calls `move_midi_inject_to_move([0x90 | 0, padNote, 100])` to fire pad (track 0, col 0). Log the wall-clock time of injection and the wall-clock time of the first audio peak from the slot.

Pad note formula: `padNote(t, c) = (92 - 8*t) + c`.

**Step 2:** Deploy and run. With Move at 120 BPM, transport playing or stopped, click the jog wheel in the middle of a bar and observe whether the clip launches immediately or waits for the next bar.

**Step 3:** Record the result in this plan and decide:
- If immediate → we need our own bar-boundary wait (host fn or read Move's clock SHM in JS).
- If quantized → Move's default launch quantization works, no extra work.

**Outcome (filled 2026-05-29, resolved by reading song-mode instead of a throwaway spike):**
**Quantized.** `move_midi_inject_to_move([0x90|t, padNote, vel])` is treated by Move
firmware as a clip launch and is **quantized to the next bar boundary** — it does NOT
fire immediately. Confirmed by song-mode's working playback engine
(`src/modules/tools/song-mode/ui.js:686`: "Move quantizes clip launches to bar
boundaries") and its pre-trigger strategy (line 693: it injects the next entry's pads
~0.25 bar / 1 beat *before* the boundary so they start on time).

Implications for Print Stems (Tasks 3.3 / 3.4):
- **No custom immediate-launch mechanism needed.** We lean on Move's bar quantization.
- The capture state machine must account for the **inject→audio latency of up to one
  bar.** Fire a column's pads ~1 beat before a bar boundary; the actual audio onset is
  the *next* boundary.
- **Timing model (mirrored from song-mode):**
  - `barDurationMs = (60000 / tempo) * 4` (4/4 assumed for v1).
  - Anchor `playStartTime = Date.now()` only after transport is confirmed running via
    `shadow_get_overlay_state().transportPlaying` (Link quantize delay; ~5 s timeout
    fallback). Bar position = `(Date.now() - playStartTime) / barDurationMs`.
  - Inject note-ons via a queue at ~50 ms spacing (`INJECT_INTERVAL_MS`).
  - **Defer note-offs ~10 ticks** after the note-on — on+off in the same MIDI_IN frame
    makes Move ignore the press.
  - Pre-warm slots with `host_wake_all_slots()` before audio arrives (avoids first-frame
    glitch under Link Audio).
- **Capture-length precision:** use the shim's monotonic block counter
  (`host_print_capture_write_index()`, 344/s) to measure pre-roll / stem / tail lengths
  in audio blocks (sample-accurate), reserving wall-clock bar math only for *when* to
  fire pads. This decouples capture accuracy from JS tick jitter.

**Step 4:** No spike module was created (resolved from existing code), so nothing to delete.
```bash
git add docs/plans/2026-05-29-print-stems.md docs/plans/2026-05-29-print-stems-design.md
git commit -m "docs(print-stems): record clip launch quantization from spike 0.2"
```

---

### Task 0.3: Inspect empty audio clipSlot shape in Set 26

**Step 1:** Pull a fresh copy of Set 26's Song.abl and find an empty audio clipSlot (any slot in tracks 0 or 1 that doesn't have a `clip` key).
```bash
scp "ableton@move.local:/data/UserData/UserLibrary/Sets/65a7e419-cd53-4e72-93d7-df6a59138597/Set 26/Song.abl" /tmp/set26_song.abl
python3 -c "import json; d=json.load(open('/tmp/set26_song.abl')); print(json.dumps(d['tracks'][0]['clipSlots'][7], indent=2))"
```

**Step 2:** Record exact JSON shape of an empty audio slot.

**Outcome (filled 2026-05-29):** Empty slot shape is `{"hasStop": true, "clip": null}` — explicit null for `clip`, `hasStop` always true. Same for audio and midi tracks; not an array hole, never missing.

**Step 3:** Also dump exact default mixer values (`pan`, `volume`, `sends`, etc.) for both audio tracks.
```bash
python3 -c "import json; d=json.load(open('/tmp/set26_song.abl')); print(json.dumps(d['tracks'][0]['mixer'], indent=2))"
```

**Outcome (filled 2026-05-29):**
```json
{
  "pan": 0.0,
  "solo-cue": false,
  "speakerOn": true,
  "volume": 0.0,
  "sends": []
}
```
Notes: `volume: 0.0` is 0 dB (Move's mixer uses dB). `solo-cue` varies per-track in Set 26 (track 0 had `true`); default to `false` for new tracks. `pan` 0 = center. `sends: []` because no return tracks defined.

Additional structural fields to copy from source `Song.abl` when generating output: `rootNote`, `scale`, `melodicLayout`, `timeSignature`, `stepEditorResolution`, `globalGrooveAmount`, `returnTracks`, `masterTrack` (includes its own devices like Compressor — keep as-is), `scenes` (array of scene objects, keep as-is), `grooves`, `metadata.usedFeatures` (extend with `"Audio Clip Properties"` if not present).

**Step 4:** Commit plan.

---

### Task 0.4: Test set-browser refresh behavior

**Step 1:** SSH to Move and copy Set 26 to a new UUID-named dir with a slightly different set name. Restart nothing.
```bash
ssh ableton@move.local '
  NEW_UUID=$(cat /proc/sys/kernel/random/uuid)
  cp -r "/data/UserData/UserLibrary/Sets/65a7e419-cd53-4e72-93d7-df6a59138597/Set 26" "/data/UserData/UserLibrary/Sets/$NEW_UUID/Set 26 Clone"
  echo "Created: $NEW_UUID"
'
```

**Step 2:** Open Move's set browser via UI. Does "Set 26 Clone" appear without restart? If no, look at what dbus signals Move's firmware emits when creating a set:
```bash
ssh ableton@move.local "dbus-monitor --system 2>&1 | head -100" &
# Then create a set on Move via UI, observe signals
```

**Step 3:** Record the refresh mechanism.

**Outcome to fill in:** _[automatic / dbus signal / requires action X]_

**Step 4:** Delete the cloned set, commit plan.
```bash
ssh ableton@move.local "rm -rf '/data/UserData/UserLibrary/Sets/$NEW_UUID'"  # replace $NEW_UUID with actual
```

---

### Task 0.5: Test sampleUri subdirs

**Step 1:** On Move, copy a stem-naming candidate file into a Recordings subdir and update Set 26's Song.abl (carefully, with a backup) to point one of its clips at the subdir path.
```bash
ssh ableton@move.local '
  cp "/data/UserData/UserLibrary/Recordings/Set 26 Rec 1 - 112 bpm.wav" "/data/UserData/UserLibrary/Recordings/test/test.wav" 2>/dev/null \
    || { mkdir -p "/data/UserData/UserLibrary/Recordings/test"; cp "/data/UserData/UserLibrary/Recordings/Set 26 Rec 1 - 112 bpm.wav" "/data/UserData/UserLibrary/Recordings/test/test.wav"; }
'
# Then manually edit Set 26's Song.abl to add a clip with sampleUri "ableton:/user-library/Recordings/test/test.wav"
```

**Step 2:** Open Set 26 on Move. Does the clip play?

**Outcome to fill in:** _[subdirs supported: yes / no]_

**Step 3:** Restore Set 26, delete test dir, commit plan.

If subdirs work, update the design doc: WAVs go in `Recordings/Schwung Stems/<setname>/` instead of flat. If not, stick with flat naming.

---

### Phase 0 deliverable

A revised plan (and possibly design doc) with **all five open questions answered** and architecture decisions locked. Do not proceed to Phase 1 until these are resolved.

---

## Phase 1 — Per-track capture infrastructure (C / shim)

Goal: expose 4 per-track pre-MFX stereo buffers so JS can read them. Output of this phase: a tested SHM read path + JS host function `host_print_capture_read(track, dst_buffer)`.

### Task 1.1: Add per-track capture SHM segment

**Files:**
- Modify: `src/host/shadow_constants.h` — add struct `shadow_print_capture_t`
- Modify: `src/schwung_shim.c` — create the SHM segment in shim init

**Step 1: Design the SHM struct.** In `src/host/shadow_constants.h`, add:

```c
/* /schwung-print-capture — 4 per-track pre-MFX stereo ring buffers.
 * Written by shim per audio block (128 frames); read by Print Stems tool.
 * Single producer (SPI callback) / single consumer (Print Stems JS thread).
 */
#define PRINT_CAPTURE_FRAMES_PER_BLOCK 128
#define PRINT_CAPTURE_NUM_TRACKS 4
#define PRINT_CAPTURE_RING_BLOCKS 64   /* ~186 ms at 44.1 kHz; plenty for JS read cadence */
#define PRINT_CAPTURE_RING_SAMPLES (PRINT_CAPTURE_FRAMES_PER_BLOCK * PRINT_CAPTURE_RING_BLOCKS * 2)

typedef struct {
    /* lock-free write_index; reader compares against last_read_index */
    volatile uint64_t write_index;       /* block count, monotonically increasing */
    uint32_t sample_rate;                /* 44100 */
    uint32_t reserved[6];
    int16_t  rings[PRINT_CAPTURE_NUM_TRACKS][PRINT_CAPTURE_RING_SAMPLES];
} shadow_print_capture_t;
```

**Step 2: Add SHM creation in shim init.** In `schwung_shim.c`, near where other SHM segments are created (search for `/schwung-link-in` or `/schwung-control`), add a `shm_open + ftruncate + mmap` for `/schwung-print-capture` of size `sizeof(shadow_print_capture_t)`. Store the pointer in a static `static shadow_print_capture_t *g_print_capture = NULL;`.

**Step 3:** Build and deploy.
```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 4: Verify SHM exists on device.**
```bash
ssh ableton@move.local "ls -la /dev/shm/schwung-print-capture"
```
Expected: a file of size `sizeof(shadow_print_capture_t)` ≈ 4 MB. If absent, check shim init log for errors.

**Step 5: Commit.**
```bash
git add src/host/shadow_constants.h src/schwung_shim.c
git commit -m "feat(shim): add /schwung-print-capture SHM for per-track audio"
```

---

### Task 1.2: Write per-track audio into the ring (Schwung slots only)

**Per Task 0.1's finding:** the source buffer is `shadow_slot_fx_deferred[s]` (post–slot-FX, pre-MFX), gated by `shadow_slot_fx_deferred_valid[s]`. The raw `shadow_slot_deferred[s]` is pre-FX (synth output only). Under `rebuild_from_la` mode the deferred-FX buffer is zeroed and slot-FX runs inside the LA rebuild branch (`schwung_shim.c:~2140`); for v1, document that LA routing currently produces silent stems and revisit in v2. Subagent should add a `LOG_DEBUG` warning when LA rebuild is active and capture is consumed, so the symptom is visible during testing.

**Files:**
- Modify: `src/schwung_shim.c` — in the SPI audio callback, after `shadow_slot_fx_deferred[s]` is populated (around line ~1708) but before MFX runs, write each track's post–slot-FX buffer into the ring.

**Step 1:** Find the section that populates `shadow_slot_fx_deferred[s]` and the `shadow_slot_fx_deferred_valid[s]` flag. Place the ring write **after** that block (so we capture the post-FX data) and **before** the LA-rebuild branch and MFX. Code:

```c
if (g_print_capture) {
    uint64_t idx = g_print_capture->write_index;
    uint64_t slot = idx % PRINT_CAPTURE_RING_BLOCKS;
    for (int t = 0; t < PRINT_CAPTURE_NUM_TRACKS; t++) {
        int16_t *dst = &g_print_capture->rings[t][slot * PRINT_CAPTURE_FRAMES_PER_BLOCK * 2];
        if (shadow_slot_fx_deferred_valid[t] && shadow_slot_fx_deferred[t]) {
            memcpy(dst, shadow_slot_fx_deferred[t], PRINT_CAPTURE_FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        } else {
            memset(dst, 0, PRINT_CAPTURE_FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        }
    }
    __atomic_store_n(&g_print_capture->write_index, idx + 1, __ATOMIC_RELEASE);
}
```

**Realtime safety:** memcpy of 4 × 512 bytes/block = 2 KB/block ≈ 45 KB/s, trivial. Atomic store is wait-free. NO logging, NO allocation, NO locks. ✅

**Step 2:** Build, deploy, verify ring is advancing.
```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
ssh ableton@move.local "python3 -c '
import mmap, struct, time
f = open(\"/dev/shm/schwung-print-capture\", \"rb\")
mm = mmap.mmap(f.fileno(), 16, prot=mmap.PROT_READ)
for _ in range(5):
    print(struct.unpack(\"Q\", mm[:8])[0])
    time.sleep(0.5)
'"
```
Expected: `write_index` advances by ~172 per 0.5s (44100/128 × 0.5).

**Step 3: Commit.**
```bash
git add src/schwung_shim.c
git commit -m "feat(shim): write Schwung slot audio into print-capture ring"
```

---

### Task 1.3: Write Move-native per-track audio into the ring

**Files:**
- Modify: `src/schwung_shim.c` — read per-track Link Audio (`link_audio_read_channel_shm` or its current API) for tracks where the Schwung slot is empty/passthrough.

**Step 1: Read Link Audio reader.** Find `link_audio_read_channel_shm` (or equivalent) in `src/host/shadow_link_audio.c` and confirm its API for reading channel N's current 128-frame block at unity. This is the same data path that feeds `unity_view` in `rebuild_from_la` mode.

**Step 2: Modify the write loop from Task 1.2.** For each track, determine whether the slot is occupied by a Schwung sound generator or routed to a Move-native track. Source from `shadow_slot_deferred[t]` in the former case, Link Audio channel `t+1` in the latter. (Or always sum both — slot output will be silent if it's a passthrough slot. Decide based on what's correct vs simple.)

Pseudo:
```c
for (int t = 0; t < PRINT_CAPTURE_NUM_TRACKS; t++) {
    int16_t track_block[PRINT_CAPTURE_FRAMES_PER_BLOCK * 2];
    if (slot_has_synth(t)) {
        memcpy(track_block, shadow_slot_deferred[t], sizeof(track_block));
    } else {
        link_audio_read_channel_into(t + 1, track_block);  /* 1-MIDI..4-MIDI */
    }
    memcpy(dst_ring_slot_for_track_t, track_block, sizeof(track_block));
}
```

(Exact function names/signatures will come from reading the Link Audio header.)

**Step 3:** Build, deploy, sanity-check with the same SHM dump from Task 1.2 — write_index should still advance; per-track audio should be non-silent when Move tracks have clips playing.

**Step 4: Audio verification (manual).** With Move at Set 26 playing track 0's audio clip (Agogo Atabaques loop), dump 1 second of track-0 ring into a WAV file via a quick Python script and listen:
```bash
ssh ableton@move.local "python3 - <<'PY' > /data/UserData/schwung/capture-test.raw
import mmap, struct
f = open('/dev/shm/schwung-print-capture', 'rb')
mm = mmap.mmap(f.fileno(), 4*1024*1024, prot=mmap.PROT_READ)
import sys; sys.stdout.buffer.write(mm[32 : 32 + 256*128*2*2])  # track 0 ring, all 64 blocks
PY"
scp "ableton@move.local:/data/UserData/schwung/capture-test.raw" /tmp/cap.raw
# Convert to WAV:
python3 -c "
import wave, sys
with wave.open('/tmp/cap.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(open('/tmp/cap.raw', 'rb').read())
"
open /tmp/cap.wav  # listen
```

Expected: ~186 ms of Agogo Atabaques audio.

**Step 5: Commit.**

---

### Task 1.4: Add JS host function `host_print_capture_read`

**Files:**
- Modify: `src/schwung_host.c` (or wherever JS host fns are registered — verify) — add the binding
- Modify: `src/host/<wherever the host_* C impls live>` — implement

**Step 1: Read existing host-fn registrations.** Grep for `JS_NewCFunction.*host_` to find the pattern.

**Step 2: Design the JS API.**
```javascript
// Returns the most recent N blocks for the given track as a typed array,
// or null if N exceeds the ring depth or audio is not advancing.
// blocks: 1..PRINT_CAPTURE_RING_BLOCKS
// Returns Int16Array of length blocks * 128 * 2 (interleaved stereo).
let buf = host_print_capture_read(track, blocks);

// Returns the current write_index (lets JS pace itself).
let idx = host_print_capture_write_index();
```

**Step 3:** Implement both as C functions that mmap `/schwung-print-capture` (cache the pointer in host state on first call). For `read`, copy from the ring into a JS-owned ArrayBuffer.

**Step 4:** Build, deploy.

**Step 5: Verify from JS.** Add a 5-line test that's removed before commit: in any tool module's `tick()`, call `host_print_capture_read(0, 1)` and log byte 0 + length. Confirm it returns sane data.

**Step 6: Commit.** (Without the test code.)

---

## Phase 2 — Song.abl I/O + loop-wrap math (host-side, TDD)

Goal: pure functions for parsing source `Song.abl`, generating output `Song.abl`, and the wrap-mix algorithm. All TDD-able in Python or node.

### Task 2.1: Set up fixtures

**Files:**
- Create: `tests/fixtures/set26-source.abl` — copy of Set 26's Song.abl (the MIDI-clip version)
- Create: `tests/fixtures/set26-stems-expected.abl` — hand-crafted "what we'd want to generate" version

**Step 1:** Copy fixture.
```bash
mkdir -p tests/fixtures
scp "ableton@move.local:/data/UserData/UserLibrary/Sets/65a7e419-cd53-4e72-93d7-df6a59138597/Set 26/Song.abl" tests/fixtures/set26-source.abl
```

**Step 2:** By hand, write `tests/fixtures/set26-stems-expected.abl`: same UUID/schema, but 4 audio tracks, all populated cells replaced with audio clips referencing `ableton:/user-library/Recordings/Set 26 Stem T<n> <letter>.wav`. Use the empty-slot shape determined in Task 0.3.

**Step 3:** Commit fixtures.

---

### Task 2.2: Parser + serializer (Python, TDD)

Why Python: it has json built in, fast to iterate, perfect for this. Output is consumed by tests only — the production code will be re-implemented in JS in Phase 6.

**Files:**
- Create: `tests/print_stems/test_abl_io.py`
- Create: `tests/print_stems/abl_io.py`

**Step 1: Write failing test for parse_clip_grid.**
```python
# test_abl_io.py
import json, pathlib
from abl_io import parse_clip_grid

def test_set26_clip_grid():
    src = json.load(open("tests/fixtures/set26-source.abl"))
    grid = parse_clip_grid(src)
    assert grid["tempo"] == 112.0
    assert grid["num_tracks"] == 4
    # Track 0 col 0 has the Agogo loop (16 beats = 4 bars)
    cell = grid["cells"][0][0]
    assert cell["beats"] == 16.0
    assert cell["loop_enabled"] is True
```

**Step 2:** Run, expect ImportError.
```bash
cd tests/print_stems && python3 -m pytest test_abl_io.py::test_set26_clip_grid -x
```

**Step 3: Implement minimal `parse_clip_grid`** — mirror the parsing logic in `src/modules/tools/song-mode/ui.js:parseSong`. Return `{tempo, num_tracks, time_signature, cells: [[{exists, beats, loop_enabled, color}, ...], ...]}`.

**Step 4:** Run, expect PASS.

**Step 5:** Add more tests covering: one-shot clips, empty cells, mixed audio/midi tracks. Implement until all pass.

**Step 6: Commit.**

---

### Task 2.3: `build_stems_song_abl` generator (TDD)

**Files:**
- Modify: `tests/print_stems/test_abl_io.py`
- Modify: `tests/print_stems/abl_io.py`

**Step 1: Write test.**
```python
def test_build_stems_song_abl_matches_fixture():
    src = json.load(open("tests/fixtures/set26-source.abl"))
    grid = parse_clip_grid(src)
    out = build_stems_song_abl(src, grid, set_name="Set 26", stem_filename_for=lambda t, c: f"Set 26 Stem T{t+1} {'ABCDEFGH'[c]}.wav")
    expected = json.load(open("tests/fixtures/set26-stems-expected.abl"))
    # Compare structurally — ignore $schema-level metadata if it differs.
    assert out["tempo"] == expected["tempo"]
    for t in range(4):
        for c in range(8):
            actual_slot = out["tracks"][t]["clipSlots"][c]
            expected_slot = expected["tracks"][t]["clipSlots"][c]
            assert actual_slot == expected_slot, f"mismatch at T{t} C{c}"
```

**Step 2:** Implement `build_stems_song_abl`:
- Copy top-level: `$schema`, `tempo`, `timeSignature`, `stepEditorResolution`, `globalGrooveAmount`, `rootNote`, `scale`, `melodicLayout`, `returnTracks`, `masterTrack`, `scenes`, `grooves`, `metadata`.
- Replace tracks: 4 entries, all `kind: "audio"`, default mixer (from Task 0.3), `devices: []`, `clipSlots` filled or empty per grid.
- For populated cells: emit the audio-clip body from the design doc, with `sampleUri` from `stem_filename_for(t, c)` (URL-encoded).

**Step 3:** Iterate until test passes.

**Step 4: Commit.**

---

### Task 2.4: URL-encoding helper for sampleUri (TDD)

**Step 1:** Test:
```python
def test_sample_uri_encoding():
    assert sample_uri("Set 26 Stem T1 A.wav") == "ableton:/user-library/Recordings/Set%2026%20Stem%20T1%20A.wav"
    assert sample_uri("My Set's Bounce.wav") == "ableton:/user-library/Recordings/My%20Set%27s%20Bounce.wav"
```

**Step 2:** Implement using `urllib.parse.quote` with `safe=""`. (Python's default `safe="/"` would leak slashes; we want the leading `Recordings/` to stay un-encoded but the filename's slashes (none expected) to encode. Use prefix concatenation.)

**Step 3: Commit.**

---

### Task 2.5: Loop-wrap algorithm (TDD with synthetic audio)

**Files:**
- Create: `tests/print_stems/test_wrap.py`
- Create: `tests/print_stems/wrap.py`

**Step 1: Test.**
```python
import numpy as np
from wrap import wrap_mix

def test_wrap_mix_zero_tail():
    stem = np.zeros(1000, dtype=np.int16)
    tail = np.zeros(2000, dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=1000)
    assert np.array_equal(out, stem)

def test_wrap_mix_decaying_tail_lands_on_head():
    L = 100
    stem = np.zeros(L, dtype=np.int16)
    tail = np.zeros(2 * L, dtype=np.int16)
    tail[0:10] = 1000   # Tail samples 0..9 should mix into stem 0..9
    tail[L:L+10] = 500  # Tail samples L..L+9 should also mix into stem 0..9
    out = wrap_mix(stem, tail, loop_length=L)
    assert out[0] == 1500
    assert out[9] == 1500
    assert out[10] == 0
```

**Step 2:** Implement `wrap_mix(stem, tail, loop_length) -> stem_out`:
```python
def wrap_mix(stem, tail, loop_length):
    out = stem.astype(np.int32).copy()
    L = loop_length
    n_chunks = len(tail) // L
    for i in range(n_chunks):
        out[:L] += tail[i*L:(i+1)*L].astype(np.int32)
    return np.clip(out, -32768, 32767).astype(np.int16)
```

**Step 3:** Run tests, iterate until pass. Add an edge-case test where `len(tail) < L` (no wrap, return stem unchanged).

**Step 4: Commit.**

---

### Phase 2 deliverable

All host-side pure functions covered by passing tests:
```bash
cd tests/print_stems && python3 -m pytest -v
```
All green.

---

## Phase 3 — Single-pass capture orchestration (JS)

Goal: a JS function `printColumnPass(col, gridCells, callbacks)` that fires pads, runs pre-roll, captures stems + tail, runs tail-clear, returns per-track audio buffers. Tested by hand on device.

### Task 3.1: Skeleton tool module

**Files:**
- Create: `src/modules/tools/print-stems/module.json`
- Create: `src/modules/tools/print-stems/ui.js`

**Step 1:** `module.json`:
```json
{
    "id": "print-stems",
    "name": "Print Stems",
    "version": "0.1.0",
    "component_type": "tool",
    "tool_config": {
        "interactive": true,
        "skip_file_browser": true
    },
    "capabilities": {
        "skip_led_clear": true
    }
}
```

**Step 2:** `ui.js` minimal:
```javascript
import { drawMessageOverlay } from '/data/UserData/schwung/shared/menu_layout.mjs';

globalThis.init = function() {
    console.log("print-stems: init");
};

globalThis.tick = function() {
    drawMessageOverlay("Print Stems\nWIP");
    host_flush_display();
};

globalThis.onMidiMessageInternal = function(data) {};
globalThis.onMidiMessageExternal = function(data) {};
```

**Step 3:** Deploy, verify it appears in Tools menu and shows the placeholder.

**Step 4: Commit.**

---

### Task 3.2: Read per-track capture from JS

**Step 1:** Add a tick-level test that calls `host_print_capture_read(0, 4)` (4 blocks = ~12 ms) and logs the buffer length. Verify on device that it returns 1024 samples per call.

**Step 2:** Replace the test with a helper:
```javascript
const FRAMES_PER_BLOCK = 128;

function readRecentAudio(track, blockCount) {
    return host_print_capture_read(track, blockCount);
}

function rmsOfBlock(int16Array) {
    let sum = 0;
    for (let i = 0; i < int16Array.length; i++) {
        sum += int16Array[i] * int16Array[i];
    }
    return Math.sqrt(sum / int16Array.length);
}

// Returns true if all 4 tracks below threshold for the last `windowBlocks` blocks.
function allTracksSilent(threshold = 30 /* ~-60 dBFS for int16 */, windowBlocks = 4) {
    for (let t = 0; t < 4; t++) {
        const buf = readRecentAudio(t, windowBlocks);
        if (!buf) return false;
        if (rmsOfBlock(buf) > threshold) return false;
    }
    return true;
}
```

**Step 3:** Add a manual test mode — jog-click logs `allTracksSilent()` result. Verify it returns true when Move is paused, false when audio plays.

**Step 4: Commit.**

---

### Task 3.3: Pad fire + bar boundary

Per Task 0.2's outcome:
- If clip launch is automatic at next-bar quantization: simply call `move_midi_inject_to_move` and we're done.
- If immediate: implement a `waitForBarBoundary()` based on the chosen mechanism.

**Step 1:** Add helpers:
```javascript
function padNote(t, c) { return (92 - 8 * t) + c; }
function silencePadNote(t) { /* returns t's empty col; for v1, hardcode col 7 or whatever song-mode uses */ }

function fireColumn(col, grid) {
    for (let t = 0; t < 4; t++) {
        const note = grid[t][col].exists ? padNote(t, col) : silencePadNote(t);
        move_midi_inject_to_move([0x90, note, 100]);
    }
}

function stopAllTracks() {
    for (let t = 0; t < 4; t++) {
        move_midi_inject_to_move([0x90, silencePadNote(t), 100]);
    }
}
```

Note: `silencePadNote` needs to know which column is empty for each track. Song-mode's `silencePads[]` already solves this; lift the logic.

**Step 2:** Manual test: tool's jog-click fires column 0 across all 4 tracks. Deploy, click, hear clips playing in parallel.

**Step 3: Commit.**

---

### Task 3.4: Single-pass capture loop

This is the big task. Use TaskCreate to track sub-steps; allow yourself to take 30+ minutes here.

**Step 1:** Write the orchestrator skeleton (no audio yet):
```javascript
async function captureColumnPass(col, grid, tempo) {
    const tracksInPass = [];
    for (let t = 0; t < 4; t++) {
        if (grid[t][col].exists) tracksInPass.push(t);
    }
    if (tracksInPass.length === 0) return [];

    const passLoopBeats = Math.max(...tracksInPass.map(t => grid[t][col].beats));
    const passLoopSamples = Math.round(passLoopBeats * (60 / tempo) * 44100);

    fireColumn(col, grid);
    // TODO: wait pre-roll
    // TODO: record stems
    // TODO: tail capture
    stopAllTracks();
    // TODO: tail-clear monitor
    // TODO: wrap-mix
    return /* { track: int16Array } map */;
}
```

JS doesn't have native async/await in QuickJS the same way; orchestrate via `tick()` with a state machine. Or use synchronous busy-wait via `host_flush_display` (NOT recommended — blocks the JS thread).

Decision: implement as a `tick()` state machine. States: `IDLE`, `PASS_FIRE`, `PASS_PREROLL`, `PASS_RECORD`, `PASS_TAIL`, `PASS_CLEAR`, `PASS_DONE`. Each tick advances based on captured-block count vs goals.

**Step 2:** Implement the state machine. Each state knows how many blocks to wait, what to do on entry/exit, and the transition condition.

**Step 3:** Plumb capture: per state, when in `PASS_RECORD` or `PASS_TAIL`, read each track's recent blocks via `readRecentAudio` and append to per-track `Int16Array`s. Use the `write_index` from `host_print_capture_write_index()` to advance without dupes/gaps.

**Step 4: Test on Set 26.** Wire a jog-click trigger that calls `captureColumnPass(0, grid, 112.0)` and on completion, calls `host_write_file("/data/UserData/schwung/test-cap-T1.wav", wavify(buffers[1]))` for each captured track. Deploy, click, copy the WAVs off the device, listen.

Expected: each file plays the clip from track T col 0, in isolation, with proper FX tails.

**Step 5: Commit.**

---

### Phase 3 deliverable

A working single-column print captures 4 isolated track stems. Manual verification on Set 26 produces playable WAVs.

---

## Phase 4 — Multi-pass orchestration + state save/restore

### Task 4.1: Initial state snapshot + restore

**Step 1:** Identify the snap/restore mechanism for:
- Move transport (probably MIDI Stop CC or a host_setting)
- Metronome (host_setting?)
- Track mute/solo (CC or set state file?)

Grep `src/shadow/shadow_ui.js`, `src/host/shadow_*.c`, settings files for clues. Document findings.

**Step 2:** Implement `snapshotState()` and `restoreState(snap)`.

**Step 3:** Wrap pass execution in try/finally so restore runs on any exit.

**Step 4:** Manual test: enter Print Stems, immediately cancel (back button), confirm everything is exactly as it was.

**Step 5: Commit.**

---

### Task 4.2: Initial tail-clear before pass 1

**Step 1:** Before pass 1, fire all silence pads and run the tail-clear monitor until `allTracksSilent()` is true (with 15s cap). Log if cap hit.

**Step 2:** Manual test: enter Print Stems while Move is playing audio with reverb, confirm pass 1 doesn't start until things go quiet.

**Step 3: Commit.**

---

### Task 4.3: Multi-pass loop

**Step 1:** Implement `printAll(grid, tempo)`:
- Snapshot state.
- Initial silence + tail-clear.
- For each column `c` where any track has a clip: `captureColumnPass(c, grid, tempo)`, accumulate per-clip captures.
- Restore state.
- Return all captures.

**Step 2:** Wire jog-click → `printAll(...)` on Set 26. Save all WAVs to `/data/UserData/schwung/test-set26-stems/T<t>_C<c>.wav`.

**Step 3:** Pull and listen to a sample of them.

**Step 4: Commit.**

---

## Phase 5 — Tool module UX

### Task 5.1: Preconditions on entry

**Step 1:** Implement the 4 precondition checks from the design doc:
1. Active set exists + parseable.
2. Set has at least 1 populated clip.
3. Free song slot OR sibling stems set exists.
4. No other module owns SPI MIDI_OUT injection. (Phase 5 acceptable to skip if no mechanism exists yet — document and move on.)

**Step 2:** Each failure → fullscreen error message + Back exits. Use `drawMessageOverlay`.

**Step 3: Commit.**

---

### Task 5.2: Overview screen

**Step 1:** Display: set name, "X/Y clips populated", "~MM:SS estimated", primary action "Print All [jog click]". Back exits.

**Step 2:** Computes estimate via the formula in the design doc (use 2s default for `expected_inter_pass_silence`).

**Step 3:** If sibling stems set already exists, append "(overwrites existing stems set)" to the screen.

**Step 4: Commit.**

---

### Task 5.3: Progress screen

**Step 1:** During `printAll`, expose state to the UI: current pass, total passes, current phase (`pre-roll` / `recording` / `tail` / `clearing`), per-track recording indicator.

**Step 2:** Tick draws: `[Pass N/M] Column D`, sub-phase, simple bar.

**Step 3:** Jog-click during progress = cancel confirmation; jog-click confirm → stop, restore state, return to overview (don't write Song.abl).

**Step 4: Commit.**

---

### Task 5.4: Complete screen

**Step 1:** After successful print + Song.abl write: "Printed M stems to <name> Stems." Jog click → call `host_return_to_menu()` or similar to open the new set if practical. Back → exit.

**Step 2: Commit.**

---

## Phase 6 — Output assembly

### Task 6.1: Re-implement parse + build in JS

**Files:**
- Create: `src/modules/tools/print-stems/abl_io.mjs`

**Step 1:** Translate `tests/print_stems/abl_io.py` parser and builder into ES module JS. Re-run the Python test logic mentally as you go.

**Step 2:** Add a JS-side smoke test (one-off, removable): in `init()`, load the source Song.abl, generate stems Song.abl with stub filenames, log a hash or length comparison. Verify on device.

**Step 3: Commit.**

---

### Task 6.2: WAV writer integration

**Step 1:** Identify the existing WAV writer (`shadow_sampler.c` has one). Expose a host fn `host_write_wav(path, int16Buffer, sampleRate, channels)` if not already available.

**Step 2:** Use it from JS to write each captured stem after wrap-mix:
```javascript
for (const { track, col, stem } of captures) {
    const filename = `${setName} Stem T${track+1} ${COL_LETTERS[col]}.wav`;
    host_write_wav(`/data/UserData/UserLibrary/Recordings/${filename}`, stem, 44100, 2);
}
```

**Step 3:** Manual verify: WAVs land in `Recordings/`, openable on the Mac.

**Step 4: Commit.**

---

### Task 6.3: Wrap-mix in JS

**Step 1:** Port `wrap_mix` from `tests/print_stems/wrap.py` to JS. Operate on Int16Array (use Int32Array for the accumulator to avoid overflow, then clip back).

**Step 2:** Unit-verify by running the same synthetic-tail test from Phase 2 — log a few sample values from a hand-crafted input.

**Step 3:** Plug into the capture loop after pass completion.

**Step 4: Commit.**

---

### Task 6.4: Generate and write Song.abl

**Step 1:** After all passes complete, generate the sibling `Song.abl` using the JS builder. Compute new UUID with whatever facility Schwung exposes (grep for `uuidgen` or `crypto.randomUUID`).

**Step 2:** Create dirs: `mkdir -p /data/UserData/UserLibrary/Sets/<new-uuid>/<setname> Stems/` via host_ensure_dir.

**Step 3:** `host_write_file(path, JSON.stringify(songAbl, null, 2))`.

**Step 4:** Trigger Move's set browser refresh per Task 0.4's outcome.

**Step 5:** Manual end-to-end test: print Set 26, open the new `Set 26 Stems` on Move, play clips, confirm they sound right.

**Step 6: Commit.**

---

### Task 6.5: Sibling-set overwrite + WAV collision handling

**Step 1:** If sibling stems set folder exists: delete it after user confirms overwrite (already prompted in Task 5.2).

**Step 2:** WAVs with same base name in `Recordings/`: just overwrite (per design doc).

**Step 3: Commit.**

---

## Phase 7 — Hardware bring-up + edge cases

### Task 7.1: Full Set 26 print

**Step 1:** From a cold boot, with Set 26 active, run Print Stems → Print All. Time it. Verify per-pass logs.

**Step 2:** Open `Set 26 Stems` on Move. Visually confirm clip layout mirrors source. Play through each clip.

**Step 3:** Open the same set in Live (import via USB). Confirm clips loop in time.

**Step 4:** If anything fails, debug and iterate.

---

### Task 7.2: Long-tail edge case

**Step 1:** Create a test set (or modify Set 26 patches) so slot 1 has a 10-second reverb. Run Print Stems. Verify pass for column with slot 1 hits the 15s hard cap and logs the warning, but other passes proceed normally.

---

### Task 7.3: Disk-full edge case

**Step 1:** Pre-fill `/data` to leave only ~10 MB free. Attempt print. Verify "Disk full" error, partial outputs cleaned (or at least no corruption).

**Step 2:** Free up space, retry, confirm clean run.

---

### Task 7.4: Cancel mid-print

**Step 1:** Start a print, jog-click cancel during pass 3 of 5. Confirm:
- Restore-state ran (transport stopped, mute/solo restored, etc.).
- No `Song.abl` written.
- Partial WAVs may or may not be in `Recordings/` — fine either way.

---

### Task 7.5: Re-print overwrite

**Step 1:** Run Print Stems twice. Confirm second run shows overwrite prompt, succeeds, replaces WAVs cleanly.

---

### Task 7.6: Song-slot-full

**Step 1:** Create enough sets to hit the cap (whatever it turns out to be). Confirm "No free song slot" precondition fires.

---

### Task 7.7: Docs + release notes

**Files:**
- Modify: `CLAUDE.md` — add Print Stems to the architecture overview and Tools menu list.
- Modify: `MANUAL.md` — user-facing explanation of how to print stems.
- Modify: `module-catalog.json` (if Print Stems ships as a catalog module) — not for v1, will ship as a built-in tool.
- Create: `src/modules/tools/print-stems/help.json` — user help screen content.
- Modify: `../schwung-catalog-site/manual.html` — keep user manual in sync per memory note.

**Step 1:** Update CLAUDE.md.

**Step 2:** Update MANUAL.md.

**Step 3:** Write help.json (see other tool modules for shape).

**Step 4:** Update catalog-site manual.

**Step 5:** Commit.

---

### Task 7.8: Final cleanup + PR

**Step 1:** Run through the design doc and confirm every accepted criterion holds.

**Step 2:** Squash exploratory commits if desired, keep semantically meaningful ones.

**Step 3:** Open PR vs `main`.

```bash
gh pr create --title "feat: Print Stems tool — bounce clips to audio-clip set" --body "$(cat <<'EOF'
## Summary
- New "Print Stems" tool under Tools menu
- Bounces every populated clip in the active set to a stereo WAV
- Emits a sibling `<name> Stems` audio-clip set ready to open on Move or import to Live
- Parallel per-track capture from pre-MFX buses; adaptive inter-pass tail-clear; loop-wrap mixdown

## Design
See `docs/plans/2026-05-29-print-stems-design.md`.

## Test plan
- [x] Phase 0 spikes resolved
- [x] Phase 2 host-side tests green
- [x] Set 26 end-to-end print
- [x] Cancel mid-print restores state
- [x] Long-tail patch hits hard cap with warning
- [x] Re-print overwrites cleanly
- [x] Song-slot-full precondition fires

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Acceptance (from design doc, copied here for executor convenience)

- Printing Set 26 produces a sibling `Set 26 Stems` set that opens in Move.
- Each populated MIDI/Schwung clip has a corresponding audio clip at the same (track, col).
- Loop boundaries are gapless.
- Snapshotted state is restored after print, regardless of outcome.
- Disk-full and "no active set" cases show a clear message.
- Fully populated set prints in ≈ `8 × (1 + 1 + 2) × max_loop_length + 8 × inter_pass_silence` seconds.

---

## Notes for the executor

- Always deploy with `./scripts/install.sh local --skip-modules --skip-confirmation` — never scp individual files.
- Realtime safety: SHM writes in the SPI callback (Phase 1) must avoid allocation, locks, and I/O. Memcpy + atomic only.
- Use `LOG_DEBUG("print-stems", "...")` in C; `console.log("print-stems: ...")` in JS.
- After each Phase, deploy and smoke-test on hardware before moving on. The cycle is slow; minimize unnecessary deploys by batching small JS changes.
- Phase 0 outcomes may invalidate later-phase assumptions. **Update this plan as those answers arrive**, before you commit downstream changes built on them.
