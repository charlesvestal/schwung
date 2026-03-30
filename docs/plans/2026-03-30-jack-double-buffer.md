# JACK Audio Double-Buffer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate the 0.34% JACK audio read miss rate by double-buffering, trading one frame of latency (~2.9ms) for zero clicks.

**Architecture:** Instead of busy-waiting for JACK to deliver audio within ~50us of being woken, snapshot JACK's completed audio at the start of each frame (in `bridge_wake`) and serve that snapshot from `bridge_read_audio`. JACK has the full ~2.9ms frame to deliver, so the snapshot is virtually always ready. The JACK driver needs no changes.

**Tech Stack:** C (schwung_jack_bridge.c), shm protocol (schwung_jack_shm.h)

---

## Background

### Current Flow (per SPI frame)

```
bridge_wake (pre-ioctl, early):
  1. Clear audio_ready flag (offset 3940)
  2. Increment frame_counter
  3. futex_wake → JACK wakes

JACK (separate thread, wakes from futex):
  4. Write fOutputBuffer → shm->audio_out (float→int16 conversion)
  5. Set audio_ready = 1

bridge_read_audio (pre-ioctl, ~50us later, inside mix_from_buffer):
  6. Busy-wait on audio_ready (up to 2M spins)
  7. Return shm->audio_out pointer
```

**Problem:** Step 6 sometimes times out — JACK scheduling latency exceeds the ~50us window. 186 misses per 55K frames = 0.34% = audible clicks.

### Double-Buffer Flow

```
bridge_wake (pre-ioctl, early):
  1. Check audio_ready — if set, snapshot shm->audio_out → local buffer
  2. Clear audio_ready
  3. Increment frame_counter, futex_wake → JACK wakes

bridge_read_audio (pre-ioctl, ~50us later):
  4. Return local buffer pointer (no waiting!)

JACK (separate thread):
  5. Write audio_out, set audio_ready = 1 (has full 2.9ms)
```

**Race-free:** The snapshot in step 1 happens BEFORE the futex_wake in step 3. JACK can only start writing AFTER step 3. So the snapshot reads completed data from the previous frame.

**Latency cost:** +1 frame = +2.9ms. Total JACK→output latency goes from ~2.9ms to ~5.8ms. Acceptable for this use case (RNBO synth/effects).

### Key Files

- `src/lib/schwung_jack_bridge.c` — bridge_wake, bridge_read_audio (THE file to change)
- `src/lib/schwung_jack_bridge.h` — public API (no changes needed)
- `src/lib/schwung_jack_shm.h` — shm layout (no changes needed)
- `src/lib/jack2/shadow/JackShadowDriver.cpp` — JACK driver (no changes needed)
- `src/schwung_shim.c` — call sites (no changes needed, but add miss tracking to snapshot)

---

## Task 1: Add double-buffer to bridge_wake and bridge_read_audio

**Files:**
- Modify: `src/lib/schwung_jack_bridge.c`

**Step 1: Add static double-buffer state**

Add these statics at the top of the file, after the `#include` block:

```c
/* Double-buffer: snapshot JACK audio in bridge_wake, serve from bridge_read_audio.
 * Eliminates busy-wait; adds +1 frame latency (~2.9ms). */
static int16_t s_jack_audio_snapshot[SCHWUNG_JACK_AUDIO_FRAMES * 2];
static int s_jack_audio_valid = 0;
```

**Step 2: Modify bridge_wake to snapshot before clearing**

Replace the entire `schwung_jack_bridge_wake` function with:

```c
void schwung_jack_bridge_wake(SchwungJackShm *shm) {
    if (!shm) return;
    if (!jack_is_active(shm)) return;

    volatile uint8_t *audio_ready = (volatile uint8_t *)(((uint8_t *)shm) + 3940);

    /* Snapshot previous frame's audio BEFORE clearing.
     * JACK is blocked on futex (hasn't been woken yet), so audio_out
     * contains completed data from the previous cycle. Safe to read. */
    if (*audio_ready) {
        memcpy(s_jack_audio_snapshot, shm->audio_out,
               SCHWUNG_JACK_AUDIO_FRAMES * 2 * sizeof(int16_t));
        s_jack_audio_valid = 1;
    }

    *audio_ready = 0;
    __sync_synchronize();

    __atomic_add_fetch(&shm->frame_counter, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &shm->frame_counter, FUTEX_WAKE, 1, NULL, NULL, 0);
}
```

**Step 3: Modify bridge_read_audio to return snapshot (no busy-wait)**

Replace the entire `schwung_jack_bridge_read_audio` function with:

```c
const int16_t *schwung_jack_bridge_read_audio(SchwungJackShm *shm) {
    if (!shm) return NULL;
    if (!jack_is_active(shm)) return NULL;
    if (!s_jack_audio_valid) return NULL;

    return s_jack_audio_snapshot;
}
```

**Step 4: Commit**

```bash
git add src/lib/schwung_jack_bridge.c
git commit -m "fix: double-buffer JACK audio to eliminate read misses

Replace busy-wait in bridge_read_audio with a snapshot taken in
bridge_wake. JACK has the full frame (~2.9ms) to deliver audio
instead of ~50us. Eliminates 0.34% miss rate at cost of +1 frame
latency (~2.9ms)."
```

---

## Task 2: Add JACK audio miss tracking to timing snapshot

Even though the double-buffer should eliminate misses, add counters so we can confirm and detect regressions.

**Files:**
- Modify: `src/lib/schwung_jack_bridge.c`
- Modify: `src/schwung_shim.c` (timing snapshot struct, logger, snapshot reset)

**Step 1: Add miss counter to bridge_wake**

In `schwung_jack_bridge_wake`, after the snapshot logic and before `*audio_ready = 0`, add a counter for when audio_ready was NOT set (JACK was late):

Add a new static and exported counter near the double-buffer statics in `schwung_jack_bridge.c`:

```c
/* Counters for monitoring — read by shim timing snapshot */
static uint32_t s_jack_audio_miss_count = 0;
static uint32_t s_jack_audio_hit_count = 0;

uint32_t schwung_jack_bridge_get_miss_count(void) { return s_jack_audio_miss_count; }
uint32_t schwung_jack_bridge_get_hit_count(void) { return s_jack_audio_hit_count; }
```

In bridge_wake, update the snapshot block:

```c
    if (*audio_ready) {
        memcpy(s_jack_audio_snapshot, shm->audio_out,
               SCHWUNG_JACK_AUDIO_FRAMES * 2 * sizeof(int16_t));
        s_jack_audio_valid = 1;
        s_jack_audio_hit_count++;
    } else if (s_jack_audio_valid) {
        /* JACK didn't finish in time — we'll serve stale audio (previous frame repeated) */
        s_jack_audio_miss_count++;
    }
```

**Step 2: Add declarations to header**

In `src/lib/schwung_jack_bridge.h`, add before `#endif`:

```c
/* Monitoring counters */
uint32_t schwung_jack_bridge_get_miss_count(void);
uint32_t schwung_jack_bridge_get_hit_count(void);
```

**Step 3: Add miss/hit tracking to shim timing snapshot**

In `src/schwung_shim.c`, add to `spi_timing_snapshot_t` (after `overrun_count`):

```c
    /* JACK audio double-buffer stats */
    uint32_t jack_audio_hits;
    uint32_t jack_audio_misses;
```

In the granular snapshot block (around line 5351, before `spi_snap.granular_ready = 1`), add:

```c
        spi_snap.jack_audio_hits = schwung_jack_bridge_get_hit_count();
        spi_snap.jack_audio_misses = schwung_jack_bridge_get_miss_count();
```

In the timing logger's granular output (around line 5420), add to the "Post(us):" log line or add a new line:

```c
            unified_log("spi_timing", LOG_LEVEL_DEBUG,
                "JACK audio: hits=%u misses=%u (%.3f%% miss)",
                spi_snap.jack_audio_hits,
                spi_snap.jack_audio_misses,
                spi_snap.jack_audio_hits > 0
                    ? (100.0 * spi_snap.jack_audio_misses /
                       (spi_snap.jack_audio_hits + spi_snap.jack_audio_misses))
                    : 0.0);
```

**Step 4: Commit**

```bash
git add src/lib/schwung_jack_bridge.c src/lib/schwung_jack_bridge.h src/schwung_shim.c
git commit -m "feat: add JACK audio hit/miss counters to timing snapshot"
```

---

## Task 3: Build and deploy

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh
```

Expected: Clean build. The changes are purely in schwung_jack_bridge.c (compiled into the shim) and schwung_shim.c.

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Verify**

SSH to device, enable logging, play audio through RNBO, check for:
1. Zero or near-zero miss count in `spi_timing` logs
2. No audible clicks
3. JACK audio still works (slight additional latency expected)

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log" | grep "JACK audio"
```

Expected log output: `JACK audio: hits=55000 misses=0 (0.000% miss)`

---

## Task 4: Investigate MIDI forwarding path for RNBO dropouts

This is an investigation task, not code changes.

**Question:** Are remaining small RNBO dropouts from our MIDI forwarding path or purely RNBO internal?

**Analysis from code review:**

The MIDI forwarding path (`midi_from_jack` → SPI MIDI_OUT buffer) runs in the **post-ioctl** callback (around line 3880 in schwung_shim.c). This is AFTER the SPI transfer, so it cannot cause audio timing issues in the current frame. The MIDI is written to the SPI output buffer for the NEXT transfer.

Key observations:
1. MIDI forwarding is gated on `overtake_mode >= 2` — only runs when RNBO is active
2. Limited to 20 events per frame (HW_MIDI_LIMIT check)
3. Sysex packets are gated during LED restore to prevent interleaving
4. The forwarding itself is just array copies — trivially fast

**Conclusion:** The MIDI forwarding path cannot cause audio dropouts. It runs post-transfer and is O(n) with n<=20 simple struct copies. Any remaining RNBO dropouts after the double-buffer fix are internal to RNBO's DSP processing (exceeding its frame budget).

To confirm: after deploying the double-buffer, check if RNBO XRun count (`jackd` logs or JACK status) correlates with dropout timing. If JACK reports no XRuns but RNBO still glitches, it's RNBO-internal.

---

## Summary

| Change | File | Impact |
|--------|------|--------|
| Double-buffer snapshot | `schwung_jack_bridge.c` | Eliminates 0.34% miss rate |
| Hit/miss counters | `schwung_jack_bridge.c/h` | Monitoring |
| Timing snapshot logging | `schwung_shim.c` | Observability |
| JACK driver | No changes | - |
| Shim call sites | No changes | - |

**Total latency impact:** +2.9ms on JACK audio path (one additional frame). The JACK path already has ~2.9ms from the previous pre-render optimization, so total becomes ~5.8ms. This is well within acceptable bounds for synth/effects use.
