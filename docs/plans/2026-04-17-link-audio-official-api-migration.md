# Link Audio — Official API Migration Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace Schwung's reverse-engineered `chnnlsv` UDP interception with Ableton's official public Link Audio API, eliminate the transport/quantum toggle workaround, and retire ~1000 lines of bespoke protocol code.

**Architecture:** Move audio reception migrates from the shim's `sendto()` hook (currently parses `chnnlsv` packets in `shadow_link_audio.c`) to the existing `link-subscriber` sidecar process. The sidecar will subscribe to Move's channels via `LinkAudioSource` callbacks and write received audio into a new shared-memory ring (`/schwung-link-in`). The shim consumes that ring via the same `link_audio_read_channel()` contract. Once reception is off the hook path, the `sendto()` hook, self-subscriber, and quantum toggle are deleted.

**Tech Stack:** Ableton Link C++ SDK (`libs/link/`), C/C++17, POSIX shared memory, existing Docker aarch64 cross-build.

**Validated on 2026-04-17:** Running upstream `link_audio_hut` on-device with no transport anywhere (Live quit, Move idle post-reboot) confirmed that a source-only peer receives audio from Move via the official API with no self-subscriber or quantum dance required. See conversation log of 2026-04-17.

---

## Background for New Contributors

### Current architecture (what you'll find in the code today)

```
Move firmware process (under LD_PRELOAD shim)
┌──────────────────────────────────────────────────┐
│ src/schwung_shim.c                               │
│   - sendto() hook (line 461)                     │
│     → link_audio_on_sendto() in shadow_link_...c │
│     → parses chnnlsv UDP packets                 │
│     → writes to link_audio.channels[i].ring[]    │  ← in-process
│   - shim reads link_audio.channels[i].ring[]     │
│     for chain routing (lines ~1191–1426)         │
│   - shim writes shadow slot audio →              │
│     /schwung-pub-audio SHM                       │
└──────────────────────────────────────────────────┘
                │                          ▲
        SHM: /schwung-pub-audio            │
                ▼                   (shim reads its
┌──────────────────────────────────┐   own rings)
│ link-subscriber sidecar process  │
│ src/host/link_subscriber.cpp     │
│   - creates LinkAudioSource      │  ← triggers Move to publish
│     subscriptions (hack)         │    (but doesn't *receive*)
│   - creates ME-Ack dummy sink    │
│   - creates LinkAudioSink per    │
│     shadow slot, reads SHM       │
└──────────────────────────────────┘
```

Key files (all paths relative to `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/`):

| File | Lines | Role |
|---|---|---|
| `src/host/link_audio.h` | 144 | Constants, SHM layout, `link_audio_state_t` struct |
| `src/host/shadow_link_audio.c` | 554 | `chnnlsv` packet parser, `link_audio_on_sendto()`, `link_audio_read_channel()` |
| `src/host/shadow_link_audio.h` | 106 | Public API for shim |
| `src/host/link_subscriber.cpp` | 391 | Sidecar: subscribes to Move, publishes shadow slots |
| `src/schwung_shim.c` | 5951 | LD_PRELOAD shim; `sendto()` hook at line 461, consumer of `link_audio.channels[]` |
| `libs/link/` | submodule | Ableton Link SDK (detached HEAD at `082691b`, pre-dates public audio API) |

### Why migrate

1. **Proven at 2026-04-17:** the new `abl_link` public C API + `pruneSendHandlers` / "receive without sink" fixes let a source-only peer receive Move's audio without any hook tricks.
2. The transport/quantum toggle dance (`link_audio_no_quantum_transport`, flag files, subscriber on/off around Start/Stop) is an artifact of the self-subscriber impersonation, not a real Link Audio requirement. We confirmed audio flows at idle.
3. Custom `chnnlsv` parsing in `shadow_link_audio.c` is brittle to Move firmware updates.
4. Shim is already overweight (realtime-critical, SPI callback budget ~900µs/frame). Offloading reception to the existing sidecar reduces shim risk.

### Device & build constraints you must respect

- **Never write to `/tmp` on Move.** Rootfs is ~100% full. Use `/data/UserData/` (~49GB).
- **Deploy with `./scripts/install.sh local --skip-modules --skip-confirmation`.** Never `scp` individual files — misses setuid bit and service restart.
- **Shim runs inside Move firmware process.** Anything you add to the shim must not call `unified_log()`, allocate, or take locks in the SPI callback path.
- **Cross-compile via Docker** (`schwung-builder` image from repo root `Dockerfile`, or `debian:bookworm` one-shot like we used for `link_audio_hut`).
- **No automated test suite.** Testing is hardware-only via SSH to `move.local`. Enable the unified logger with `touch /data/UserData/schwung/debug_log_on` and `tail -f /data/UserData/schwung/debug.log`.

### What we already proved works (2026-04-17)

Binary: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/ableton-link-upstream/build-arm64-fullstatic/bin/link_audio_hut`, deployed to `/data/UserData/schwung/experiments/link_audio_hut` on device.

- Discovers `Move | 1-MIDI`..`4-MIDI`, `Move | Main`, `ME | ME-1..4`, `ME | ME-Ack`, `ME | ME-Master`
- Subscribing to `Move | Main` via `abl_link_audio_source_create` returns live audio buffers (~35 buffers per 200ms)
- Audio starts ~7s after subscription — unknown whether this is Link session negotiation latency or something else (investigate in Phase 8)
- Works with zero transport running anywhere

---

## Feature flag strategy

Every phase that changes runtime behavior gates the change behind a new flag in `/data/UserData/schwung/features.json` read at shim startup, so we can roll back on hardware instantly. The current flag `"link_audio_enabled": true` stays and means "Link Audio subsystem is live." We add:

- `"link_audio_receive_via_sidecar"` (Phase 4) — when true, shim reads Move audio from the new SHM instead of from the `sendto`-parsed rings.
- After Phase 5 this flag becomes the only path and is removed.

---

## Phase 0 — Prep & submodule bump

Goal: get the public `abl_link` audio API into `libs/link/` without breaking today's build.

### Task 0.1: Check current shim builds clean

**Files:** none modified

**Step 1:** Run the full build once to establish baseline.

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh 2>&1 | tail -40
```

Expected: ends with `Build complete!` and `schwung.tar.gz` created. If this fails, STOP — the plan assumes a working baseline.

**Step 2:** Deploy and confirm current Link Audio still works.

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'
```

Open Live, enable its Link Audio, verify Move's 5 channels appear as peer channels, verify shadow slots appear as "ME". Snapshot of working state before we touch anything.

**Step 3:** Commit nothing — this is baseline verification.

### Task 0.2: Bump `libs/link` submodule to upstream master

**Files:**
- Modify: `libs/link` submodule pointer
- Modify: `.gitmodules` (if branch tracking added)

**Step 1:** Inside the submodule, fetch upstream and check which commits we'd pick up.

```bash
cd libs/link
git fetch origin
git log --oneline HEAD..origin/master | head -30
```

Expected: you see commits including `1380ccbc Extend abl_link with an audio API`, `03ad22cd Add link_audio_hut`, `2ea60ca0 Fix receiving without Sink`, `1676fb8f Fix pruneSendHandlers not running for source-only peers`. These are what we need.

**Step 2:** Check if the current fork-branch carries local commits we must preserve. The submodule's `master` branch currently shows `ahead 2, behind 6` relative to upstream.

```bash
git log --oneline origin/master..master
```

If this prints anything, those are local commits. **Stop and ask** before overwriting — they may be needed for today's `link_subscriber.cpp` build. Document the shas in the plan execution notes and defer resolution.

If empty (or commits are trivially reproducible), proceed.

**Step 3:** Move the submodule pointer to upstream master tip.

```bash
git checkout origin/master
cd ../..
git add libs/link
```

**Step 4:** Verify the shim still builds after the bump.

```bash
./scripts/build.sh 2>&1 | tail -40
```

Expected: still succeeds. The current `link_subscriber.cpp` uses `ableton::LinkAudioSink`/`LinkAudioSource` (internal C++ API). Check whether these still exist or were renamed under `abl_link/`. If they were removed upstream, the build will fail here — that's fine, note the breakage and proceed to Task 0.3.

**Step 5:** Commit.

```bash
git commit -m "chore: bump libs/link submodule to upstream master for abl_link audio API"
```

### Task 0.3: Resolve any `link_subscriber.cpp` build breakage from Task 0.2

If Task 0.2 step 4 succeeded, skip this task.

**Files:**
- Modify: `src/host/link_subscriber.cpp`
- Possibly modify: `scripts/build.sh`

**Step 1:** Identify every upstream symbol name that changed. Grep the submodule for the replaced class names.

```bash
grep -r "class LinkAudioSource\|class LinkAudioSink" libs/link/include/ libs/link/src/ 2>/dev/null
```

**Step 2:** For each removed symbol, find its replacement in the new header `libs/link/extensions/abl_link/include/abl_link.h`. Document the map in a short comment at the top of `link_subscriber.cpp`.

**Step 3:** Apply minimal edits to `link_subscriber.cpp` — we are not migrating to the C API yet, only restoring the build. Keep scope tight.

**Step 4:** Verify.

```bash
./scripts/build.sh 2>&1 | grep -E "link-subscriber|error"
```

**Step 5:** Commit.

```bash
git commit -m "fix: adapt link_subscriber.cpp to renamed Link SDK symbols"
```

### Task 0.4: Re-deploy and verify no regression

**Step 1:** Deploy and smoke-test that today's hybrid sendto-hook flow still works after the bump.

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 2:** Open Live, verify Move channels and ME slots still appear as Link Audio peers. If broken, roll back the submodule:

```bash
git revert HEAD~N..HEAD   # the Phase-0 commits
```

Only proceed past Phase 0 once baseline is green on-device.

---

## Phase 1 — New shared-memory contract for Move→shim audio

Goal: define the SHM layout that the sidecar will write into and the shim will read from. Ship just the header + creation code, nothing consumes it yet.

### Task 1.1: Add `link_audio_in_shm_t` to `src/host/link_audio.h`

**Files:**
- Modify: `src/host/link_audio.h`

**Step 1:** Read the existing pub-side SHM types at `src/host/link_audio.h:48-78` to mirror the pattern. We want symmetrically: one slot per Move channel, SPSC ring, stereo int16.

**Step 2:** Append to the header (before `#endif`):

```c
/* ============================================================================
 * Move → shim audio shared memory (written by link-subscriber sidecar,
 * read by shim). Replaces the sendto()-hook + in-process channel rings.
 * ============================================================================ */

#define SHM_LINK_AUDIO_IN  "/schwung-link-in"

/* Use the same block size as the pub side for symmetry. */
#define LINK_AUDIO_IN_BLOCK_FRAMES   LINK_AUDIO_PUB_BLOCK_FRAMES
#define LINK_AUDIO_IN_BLOCK_SAMPLES  LINK_AUDIO_PUB_BLOCK_SAMPLES
#define LINK_AUDIO_IN_RING_BLOCKS    LINK_AUDIO_PUB_SHM_BLOCKS
#define LINK_AUDIO_IN_RING_SAMPLES   (LINK_AUDIO_IN_BLOCK_SAMPLES * LINK_AUDIO_IN_RING_BLOCKS)
#define LINK_AUDIO_IN_RING_MASK      (LINK_AUDIO_IN_RING_SAMPLES - 1)

/* One slot per Move-published channel: 1-MIDI..4-MIDI + Main = 5. */
#define LINK_AUDIO_IN_SLOT_COUNT   LINK_AUDIO_MOVE_CHANNELS
#define LINK_AUDIO_IN_MAIN_IDX     (LINK_AUDIO_MOVE_CHANNELS - 1)

typedef struct {
    int16_t  ring[LINK_AUDIO_IN_RING_SAMPLES];  /* stereo interleaved */
    volatile uint32_t write_pos;                /* sidecar (producer) */
    volatile uint32_t read_pos;                 /* shim (consumer) */
    volatile int      active;                   /* 1 once first packet received */
    char     name[32];                          /* "1-MIDI", "Main", … */
} link_audio_in_slot_t;

typedef struct {
    volatile uint32_t magic;    /* 0x4C41494E = "LAIN" */
    volatile uint32_t version;  /* 1 */
    link_audio_in_slot_t slots[LINK_AUDIO_IN_SLOT_COUNT];
} link_audio_in_shm_t;

#define LINK_AUDIO_IN_SHM_MAGIC   0x4C41494E
#define LINK_AUDIO_IN_SHM_VERSION 1
```

**Step 3:** Build the shim to confirm the header still compiles.

```bash
./scripts/build.sh 2>&1 | grep -E "error|warning: implicit"
```

Expected: no new errors.

**Step 4:** Commit.

```bash
git add src/host/link_audio.h
git commit -m "feat: define link_audio_in_shm_t for sidecar→shim audio path"
```

### Task 1.2: Create the SHM segment in sidecar at startup

**Files:**
- Modify: `src/host/link_subscriber.cpp`

**Step 1:** Add an `open_or_create_in_shm()` helper near the existing `open_pub_shm()` (around line 95). It should `shm_open` with `O_CREAT`, `ftruncate` to `sizeof(link_audio_in_shm_t)`, `mmap`, and initialize magic/version if freshly created.

```cpp
static link_audio_in_shm_t *open_or_create_in_shm()
{
    int fd = shm_open(SHM_LINK_AUDIO_IN, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, sizeof(link_audio_in_shm_t)) < 0) { close(fd); return nullptr; }
    auto *shm = (link_audio_in_shm_t *)mmap(nullptr, sizeof(link_audio_in_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) return nullptr;
    if (shm->magic != LINK_AUDIO_IN_SHM_MAGIC) {
        memset(shm, 0, sizeof(*shm));
        shm->magic = LINK_AUDIO_IN_SHM_MAGIC;
        shm->version = LINK_AUDIO_IN_SHM_VERSION;
    }
    return shm;
}
```

**Step 2:** Call it once in `main()` near where the Link session is created. Store the pointer in a local variable. Log success/failure via `LOG_INFO(LINK_SUB_LOG_SOURCE, "in shm opened/created")`.

**Step 3:** Build & deploy.

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 4:** Verify on device.

```bash
ssh ableton@move.local 'ls /dev/shm/ | grep schwung'
```

Expected output includes `schwung-link-in` alongside the existing `schwung-pub-audio`, `schwung-audio`, etc.

**Step 5:** Commit.

```bash
git commit -m "feat: link-subscriber creates /schwung-link-in shm segment"
```

---

## Phase 2 — Sidecar writes Move audio into SHM

Goal: the sidecar is already creating `LinkAudioSource` subscriptions. We hook into its callbacks to write received audio into `/schwung-link-in`. Nothing reads it yet — we verify via `hexdump` on-device.

### Task 2.1: In the source callback, find which Move channel index this is

**Files:**
- Modify: `src/host/link_subscriber.cpp`

**Step 1:** Read the current source-creation block (around lines 230–250). The callback today is a lambda that discards the buffer (`[](LinkAudioSource::BufferHandle){}`). We need to replace it with a handler that knows which slot the buffer is for.

**Step 2:** When iterating channels to subscribe, record the peer name, channel name, and the slot index we assigned. A `std::array<std::string, LINK_AUDIO_IN_SLOT_COUNT>` keyed by slot name works fine. Example mapping:

| Move channel name | `link_audio_in_shm_t.slots[i]` |
|---|---|
| `1-MIDI` | 0 |
| `2-MIDI` | 1 |
| `3-MIDI` | 2 |
| `4-MIDI` | 3 |
| `Main`   | 4 (= `LINK_AUDIO_IN_MAIN_IDX`) |

Only accept channels where `peer_name == "Move"`. Drop everything else.

**Step 3:** Write each slot name into `in_shm->slots[i].name` the first time you claim a slot.

**Step 4:** Build & deploy. No behavior change yet — log the slot assignments via `LOG_INFO` and read them via `debug.log`.

**Step 5:** Commit.

```bash
git commit -m "feat: link-subscriber resolves Move channels to in-shm slot indices"
```

### Task 2.2: Write audio samples into the SHM ring from the source callback

**Files:**
- Modify: `src/host/link_subscriber.cpp`

**Step 1:** Read `on_source_buffer` in upstream `extensions/abl_link/examples/link_audio_hut/main.c` as a reference for the callback signature and buffer layout. The buffer is `int16_t samples[num_frames * num_channels]` plus `num_frames`, `num_channels`, `sample_rate`.

**Step 2:** Implement the write path as an SPSC ring write (no locks, no logging inside the callback — it's on Link's audio thread). Pattern:

```cpp
/* In the lambda/callback: */
const uint32_t to_copy = num_frames * num_channels;   /* samples, not bytes */
uint32_t wp = slot->write_pos;
for (uint32_t i = 0; i < to_copy; ++i) {
    slot->ring[(wp + i) & LINK_AUDIO_IN_RING_MASK] = samples[i];
}
__atomic_store_n(&slot->write_pos, wp + to_copy, __ATOMIC_RELEASE);
__atomic_store_n(&slot->active, 1, __ATOMIC_RELAXED);
```

If `num_channels != 2`, drop the buffer (log once outside the callback if we see it). Move always publishes stereo; we assert that assumption explicitly.

**Step 3:** Build & deploy. Subscribe should still work exactly as before (shim still reading its own in-process rings via the `sendto` hook).

**Step 4:** Verify sidecar is writing to the SHM from the device:

```bash
ssh ableton@move.local '
  sleep 3  # let subscriber start
  hexdump -C /dev/shm/schwung-link-in | head -5
  echo "---"
  # Check write_pos has advanced — offset of write_pos is
  # 4 (magic) + 4 (version) + sizeof(ring) for slot 0
  '
```

Expected: ring memory contains non-zero audio samples. If zeros, the callback isn't firing — check logs.

**Step 5:** Commit.

```bash
git commit -m "feat: link-subscriber writes Move audio to /schwung-link-in"
```

---

## Phase 3 — Shim reads Move audio from SHM (behind flag)

Goal: add a parallel read path in the shim, gated by `link_audio_receive_via_sidecar`, that reads from `/schwung-link-in` instead of the in-process `link_audio.channels[].ring[]`.

### Task 3.1: Add feature flag and flag parsing

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1:** Read the existing `link_audio_enabled` parse at `src/schwung_shim.c:675-685` for the pattern. Add a sibling flag `link_audio_receive_via_sidecar` (default false). Store in a global `volatile int link_audio_receive_via_sidecar_flag = 0;`.

**Step 2:** Update the features log line at `src/schwung_shim.c:729-731` to include the new flag.

**Step 3:** Build & deploy, verify log shows the new flag as `disabled` by default.

**Step 4:** Commit.

```bash
git commit -m "feat: add link_audio_receive_via_sidecar feature flag (default off)"
```

### Task 3.2: Open `/schwung-link-in` as a consumer in the shim

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1:** Near the existing `shm_pub_audio_fd = shm_open(SHM_LINK_AUDIO_PUB, …)` at line 2300, add a symmetric read-only open for `/schwung-link-in`. Use `O_RDONLY`, map read-only, check magic. If the segment doesn't exist yet (sidecar may not have started), retry periodically from a non-RT context; do **not** fail shim startup.

**Step 2:** Store pointer in `static link_audio_in_shm_t *shadow_in_audio_shm = NULL;`.

**Step 3:** Build & deploy. Log "in shm opened" on successful attach. Behavior unchanged (nothing reads the pointer yet).

**Step 4:** Commit.

```bash
git commit -m "feat: shim attaches to /schwung-link-in (read-only, gated)"
```

### Task 3.3: New `link_audio_read_channel_shm()` consumer

**Files:**
- Modify: `src/host/shadow_link_audio.c`, `src/host/shadow_link_audio.h`

**Step 1:** Read the existing `link_audio_read_channel()` (used from `schwung_shim.c` line ~1426) to understand the contract: it takes a slot index and an out buffer of `frames * 2` samples, returns 1 on success.

**Step 2:** Add a parallel function:

```c
int link_audio_read_channel_shm(link_audio_in_shm_t *shm, int slot_idx,
                                int16_t *out_lr, int frames);
```

SPSC read, advances `read_pos`, returns 0 if no data available (caller zeroes buffer).

**Step 3:** Keep the old `link_audio_read_channel()` unchanged. Add the new function alongside.

**Step 4:** Build, verify no warnings.

**Step 5:** Commit.

```bash
git commit -m "feat: add link_audio_read_channel_shm() consumer helper"
```

### Task 3.4: Route shim consumers through the flag

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1:** Find every call to `link_audio_read_channel(s, ...)` in `schwung_shim.c`. There are likely 2–3. Replace each with:

```c
int ok;
if (link_audio_receive_via_sidecar_flag && shadow_in_audio_shm) {
    ok = link_audio_read_channel_shm(shadow_in_audio_shm, s, la_cache[s], FRAMES_PER_BLOCK);
} else {
    ok = link_audio_read_channel(s, la_cache[s], FRAMES_PER_BLOCK);
}
```

**Step 2:** Find every read of `link_audio.move_channel_count` used for gating. Add the sidecar path: when sidecar mode is on, use a derived count based on `shadow_in_audio_shm->slots[i].active`.

**Step 3:** Build. No deploy yet.

**Step 4:** Commit.

```bash
git commit -m "feat: shim routes move-audio reads through receive-via-sidecar flag"
```

### Task 3.5: On-device A/B validation

**Step 1:** Deploy with flag **off** (default). Verify current behavior is intact:
- `ssh ableton@move.local 'cat /data/UserData/schwung/features.json'` shows no `link_audio_receive_via_sidecar` or it's `false`.
- Open Live, play Move pads, verify shadow FX still processes Move audio (e.g., slot FX wet output).

**Step 2:** Turn on the flag:

```bash
ssh ableton@move.local "sed -i 's/\"link_audio_enabled\": *true/\"link_audio_enabled\": true, \"link_audio_receive_via_sidecar\": true/' /data/UserData/schwung/features.json"
ssh ableton@move.local 'systemctl restart schwung' || ssh ableton@move.local 'reboot'
```

**Step 3:** Re-test the same scenarios. Move audio should still flow into shadow FX — now via `/schwung-link-in` instead of the sendto hook. Check logs for "receive-via-sidecar: slot N active" debug lines.

**Step 4:** If broken, flip flag off via the same `sed`, restart, and debug.

**Step 5:** Commit & document validation result.

```bash
git commit -m "docs: record on-device validation of receive-via-sidecar flag"
```

---

## Phase 4 — Make sidecar path the default; keep fallback

### Task 4.1: Default the flag to true

**Files:**
- Modify: `src/schwung_shim.c` (flag default)
- Modify: sample `features.json` in `resources/` if one exists

**Step 1:** Change the default in the flag parser to `1`.

**Step 2:** Deploy, verify flag reads as enabled by default on devices that don't have it set.

**Step 3:** Commit.

```bash
git commit -m "feat: default link_audio_receive_via_sidecar to enabled"
```

### Task 4.2: Document the migration in CLAUDE.md and MEMORY.md

**Files:**
- Modify: `CLAUDE.md` (Link Audio section)
- Modify: `memory/link_audio_wire_format.md` (via Claude memory system, not checked in)

**Step 1:** Update the Link Audio section in `CLAUDE.md` to describe the sidecar-owns-reception model. Note that `sendto` hook is still present but unused when the flag is on.

**Step 2:** Commit.

```bash
git commit -m "docs: update CLAUDE.md for sidecar-owned Link Audio reception"
```

---

## Phase 5 — Delete the `sendto` hook path

Goal: now that the flag has defaulted-on for a release cycle and shipped cleanly, remove the legacy path entirely.

### Task 5.1: Delete sendto hook and chnnlsv parser

**Files:**
- Modify: `src/schwung_shim.c` — remove `sendto()` override (around line 461), remove `real_sendto` dlsym, remove calls to `link_audio_on_sendto`
- Delete: `src/host/shadow_link_audio.c` — the chnnlsv parser; keep `link_audio_read_channel_shm` and move it to a new smaller file, e.g. `src/host/link_audio_shm.c`
- Modify: `src/host/shadow_link_audio.h` — trim to just the SHM reader API
- Modify: `scripts/build.sh` lines 225, 247 — remove `shadow_link_audio.c` from shim sources, add new file

**Step 1:** Start with build.sh to make the compilation unit list right.

**Step 2:** Delete `link_audio_on_sendto()` call sites and declarations.

**Step 3:** Remove the `sendto` override function wholesale.

**Step 4:** Move the in-process `link_audio_state_t.channels[]` rings, the `sequence` counters, the `packets_intercepted` stats to either deletion or a simple "packets received via sidecar" rename — pick deletion unless we actually use them elsewhere.

**Step 5:** Build. Expect clean.

**Step 6:** Deploy, re-verify on-device.

**Step 7:** Commit.

```bash
git commit -m "refactor: delete sendto-hook chnnlsv parser, sidecar owns receive path"
```

### Task 5.2: Delete the feature flag (path is now the only path)

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1:** Remove flag parse and every guard on `link_audio_receive_via_sidecar_flag`.

**Step 2:** Build, deploy, verify.

**Step 3:** Commit.

```bash
git commit -m "chore: remove link_audio_receive_via_sidecar flag (now always on)"
```

---

## Phase 6 — Remove transport/quantum toggle

Goal: the sidecar's subscriber-toggle-around-transport logic (`MEMORY.md` references memory `link_audio_no_quantum_transport`) is dead weight now — we confirmed Move publishes at idle.

### Task 6.1: Identify and delete quantum toggle code

**Files:**
- Modify: `src/host/link_subscriber.cpp`
- Possibly modify: `src/shadow/shadow_ui.js` (if it writes flag files)
- Possibly modify: `src/schwung_shim.c` (if it watches flag files)

**Step 1:** Search for the flag-file plumbing:

```bash
grep -rn "no_quantum\|transport_toggle\|subscriber_enabled\|link_transport_flag" src/
```

**Step 2:** Delete the file-watching loop and the re-subscribe/disconnect calls that toggle source creation based on transport state. Replace with unconditional "create sources for discovered Move channels."

**Step 3:** Deploy, verify audio still flows at idle and during playback and across Live start/stop.

**Step 4:** Commit.

```bash
git commit -m "refactor: remove quantum/transport toggle workaround"
```

---

## Phase 7 — (Optional) Migrate sidecar to public `abl_link` C API

Goal: the sidecar currently uses `ableton::LinkAudioSource`/`LinkAudioSink` from `<ableton/LinkAudio.hpp>` — the internal C++ API that predates the now-public C API. Switching makes the sidecar a pure C translation unit, removes the need for `-static-libstdc++`, and aligns with Ableton's supported surface.

Treat this phase as **defer until needed**. Only do it if:
- We need new behavior only present in the C API (peer-name getter, realtime-safe channel query, etc.)
- The internal C++ API is removed upstream

### Task 7.1: Rewrite `link_subscriber.cpp` → `link_subscriber.c` against `abl_link.h`

**Files:**
- Create: `src/host/link_subscriber.c`
- Delete: `src/host/link_subscriber.cpp`
- Modify: `scripts/build.sh` lines 312–346 — swap g++ for gcc, drop `-std=c++17`, drop `-static-libstdc++`

Refer to `libs/link/extensions/abl_link/examples/link_audio_hut/main.c` as the idiomatic example. The `link_audio_hut` binary we built and deployed under `/data/UserData/schwung/experiments/` works as a reference for peer discovery, source creation, and sink creation.

Bite-sized sub-tasks are TBD depending on SHM-wiring details — split into: session init, channels-changed callback, source creation per Move channel, sink creation per shadow slot, SHM read loop feeding sinks.

---

## Phase 8 — Investigate the ~7s first-audio warmup

Goal: `link_audio_hut` shows a gap of ~7s between `abl_link_audio_source_create()` and the first received buffer. Current Schwung behavior gets audio much sooner via `sendto` interception. We need to understand why before we can claim parity.

### Task 8.1: Instrument the sidecar

**Files:**
- Modify: `src/host/link_subscriber.cpp`

**Step 1:** Log a high-resolution timestamp when `LinkAudioSource` is created and when the first buffer arrives for each slot.

**Step 2:** Log per-second peer/channel counts to see if the list is stabilizing slowly.

**Step 3:** Deploy, collect 10 cold-boot traces, summarize the distribution in `docs/link-audio-warmup.md`.

### Task 8.2: Mitigations (only if warmup is actually user-visible)

Candidates — evaluate each only if Task 8.1 shows the warmup actually impacts the user:

- Pre-subscribe at shim startup (before user tries to use shadow FX) — amortizes the cost.
- Investigate whether `abl_link_audio_source_create` can be retried aggressively.
- File an issue upstream if the delay is a Link SDK bug.

---

## Rollback plan

| Phase | How to roll back |
|---|---|
| 0 | Revert submodule pointer commit. |
| 1 | No runtime effect — SHM only created, not consumed. |
| 2 | Sidecar writes to SHM but shim ignores it unless flag set. Remove sidecar write code to fully revert. |
| 3 | Flip `link_audio_receive_via_sidecar` to `false` in `features.json`, restart shim. |
| 4 | Same, but `features.json` must explicitly set the flag `false` since default flipped. |
| 5 | Revert the deletion commits (re-adds sendto hook + chnnlsv parser). |
| 6 | Revert the quantum-toggle deletion. |

---

## Out of scope (not in this plan)

- Remote/off-device Link Audio listeners. The new public API makes this trivial but is a follow-up.
- Using the abl_link tempo/session-state for Move's own transport sync. That's a separate plan.
- Exposing Move audio to JS modules via a host API. Separate plan.
- Replacing `/schwung-pub-audio` — it already works; only the IN-direction SHM is new.
