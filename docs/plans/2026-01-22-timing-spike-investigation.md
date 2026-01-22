# Timing Spike Investigation & Mitigation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Identify what causes pre-ioctl processing spikes (15µs avg → 141µs max) and implement mitigations to prevent crashes.

**Architecture:** Stock Move uses 97% of its 2.9ms audio callback budget for SPI. Our shim adds ~16µs average but spikes to 141µs. We'll add granular timing to identify the bottleneck, then implement graceful degradation (frame skipping) as a safety net.

**Tech Stack:** C, ARM64 cross-compilation, LD_PRELOAD shim

---

## Background

### Timing Budget
- **Total budget:** 2900µs (128 frames @ 44.1kHz)
- **Stock Move SPI:** 2806µs average, 2877µs max (measured on DFU-restored device)
- **Remaining headroom:** ~94µs
- **Our pre-ioctl processing:** 15µs average, **141µs max** (10x spike)

### Current Pre-ioctl Processing Order
1. `midi_monitor()` - hotkey detection (~1-2µs)
2. `shadow_forward_midi()` - MIDI filtering, has `access()` syscalls (~2-5µs)
3. `shadow_mix_audio()` - triple buffer read (~1-2µs)
4. `shadow_inprocess_handle_ui_request()` - can call plugin set_param (variable)
5. `shadow_inprocess_handle_param_request()` - can call plugin get_param/set_param (variable)
6. `shadow_inprocess_process_midi()` - MIDI routing (~1-2µs)
7. `shadow_inprocess_mix_audio()` - **DSP rendering for 4 slots + 4 master FX** (variable)
8. Volume capture slice logic - memcpy operations (~2-5µs)
9. Shift+knob overlay compositing (~1-2µs)
10. `shadow_swap_display()` - display buffer swap (~1µs)

### Suspected Spike Causes
- DSP plugin `render_block()` (most likely)
- Plugin `set_param()` / `get_param()` (file I/O, memory allocation)
- `access()` syscalls on first check cycle
- Occasional GC/memory pressure

---

## Task 1: Add Granular Pre-ioctl Timing

**Files:**
- Modify: `src/move_anything_shim.c:4136-4200` (pre-ioctl section)

**Step 1: Add timing variables at top of ioctl function**

Add after existing timing declarations (~line 4120):

```c
/* === GRANULAR PRE-IOCTL TIMING === */
static struct timespec section_start, section_end;
static uint64_t midi_mon_sum = 0, midi_mon_max = 0;
static uint64_t fwd_midi_sum = 0, fwd_midi_max = 0;
static uint64_t mix_audio_sum = 0, mix_audio_max = 0;
static uint64_t ui_req_sum = 0, ui_req_max = 0;
static uint64_t param_req_sum = 0, param_req_max = 0;
static uint64_t proc_midi_sum = 0, proc_midi_max = 0;
static uint64_t inproc_mix_sum = 0, inproc_mix_max = 0;
static uint64_t display_sum = 0, display_max = 0;
static int granular_count = 0;
```

**Step 2: Wrap each pre-ioctl section with timing**

Replace the pre-ioctl processing section with timed versions:

```c
/* === PRE-IOCTL PROCESSING WITH GRANULAR TIMING === */

#define TIME_SECTION_START() clock_gettime(CLOCK_MONOTONIC, &section_start)
#define TIME_SECTION_END(sum, max) do { \
    clock_gettime(CLOCK_MONOTONIC, &section_end); \
    uint64_t _us = (section_end.tv_sec - section_start.tv_sec) * 1000000 + \
                   (section_end.tv_nsec - section_start.tv_nsec) / 1000; \
    sum += _us; \
    if (_us > max) max = _us; \
} while(0)

TIME_SECTION_START();
midi_monitor();
TIME_SECTION_END(midi_mon_sum, midi_mon_max);

/* Check if shadow UI requested exit via shared memory */
if (shadow_control && shadow_display_mode && !shadow_control->display_mode) {
    shadow_display_mode = 0;
    shadow_inject_knob_release = 1;
}

TIME_SECTION_START();
shadow_forward_midi();
TIME_SECTION_END(fwd_midi_sum, fwd_midi_max);

TIME_SECTION_START();
shadow_mix_audio();
TIME_SECTION_END(mix_audio_sum, mix_audio_max);

#if SHADOW_INPROCESS_POC
TIME_SECTION_START();
shadow_inprocess_handle_ui_request();
TIME_SECTION_END(ui_req_sum, ui_req_max);

TIME_SECTION_START();
shadow_inprocess_handle_param_request();
TIME_SECTION_END(param_req_sum, param_req_max);

TIME_SECTION_START();
shadow_inprocess_process_midi();
TIME_SECTION_END(proc_midi_sum, proc_midi_max);

TIME_SECTION_START();
shadow_inprocess_mix_audio();
TIME_SECTION_END(inproc_mix_sum, inproc_mix_max);
#endif

/* Skip timing for slice capture and overlay - group with display */
TIME_SECTION_START();
/* ... existing volume capture and overlay code ... */
shadow_swap_display();
TIME_SECTION_END(display_sum, display_max);
```

**Step 3: Add granular timing log output**

Add before the existing timing log output (~line 4780):

```c
/* Log granular timing every 1000 blocks */
granular_count++;
if (granular_count >= 1000) {
    FILE *f = fopen("/tmp/ioctl_timing.log", "a");
    if (f) {
        fprintf(f, "Granular (1000 blocks): midi_mon avg=%llu max=%llu | fwd_midi avg=%llu max=%llu | "
                   "mix_audio avg=%llu max=%llu | ui_req avg=%llu max=%llu | param_req avg=%llu max=%llu | "
                   "proc_midi avg=%llu max=%llu | inproc_mix avg=%llu max=%llu | display avg=%llu max=%llu\n",
                (unsigned long long)(midi_mon_sum / granular_count), (unsigned long long)midi_mon_max,
                (unsigned long long)(fwd_midi_sum / granular_count), (unsigned long long)fwd_midi_max,
                (unsigned long long)(mix_audio_sum / granular_count), (unsigned long long)mix_audio_max,
                (unsigned long long)(ui_req_sum / granular_count), (unsigned long long)ui_req_max,
                (unsigned long long)(param_req_sum / granular_count), (unsigned long long)param_req_max,
                (unsigned long long)(proc_midi_sum / granular_count), (unsigned long long)proc_midi_max,
                (unsigned long long)(inproc_mix_sum / granular_count), (unsigned long long)inproc_mix_max,
                (unsigned long long)(display_sum / granular_count), (unsigned long long)display_max);
        fclose(f);
    }
    midi_mon_sum = midi_mon_max = fwd_midi_sum = fwd_midi_max = 0;
    mix_audio_sum = mix_audio_max = ui_req_sum = ui_req_max = 0;
    param_req_sum = param_req_max = proc_midi_sum = proc_midi_max = 0;
    inproc_mix_sum = inproc_mix_max = display_sum = display_max = 0;
    granular_count = 0;
}
```

**Step 4: Build and deploy**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything
./scripts/build.sh && ./scripts/install.sh local
```

**Step 5: Collect data**

```bash
ssh ableton@move.local "tail -f /tmp/ioctl_timing.log"
```

Play some notes, load patches, use the UI. Look for which section has the highest `max` value.

**Step 6: Commit**

```bash
git add src/move_anything_shim.c
git commit -m "feat: add granular pre-ioctl timing instrumentation"
```

---

## Task 2: Implement Graceful Degradation (Frame Skipping)

**Files:**
- Modify: `src/move_anything_shim.c:4136-4200` (pre-ioctl section)

This is **required in any state** - it's our safety net regardless of what causes spikes.

**Step 1: Add overrun detection variables**

Add near other timing variables:

```c
/* === OVERRUN DETECTION AND RECOVERY === */
static int consecutive_overruns = 0;
static int skip_dsp_this_frame = 0;
static uint64_t last_frame_total_us = 0;
#define OVERRUN_THRESHOLD_US 2850  /* Start worrying at 2850µs */
#define SKIP_DSP_THRESHOLD 3       /* Skip DSP after 3 consecutive overruns */
```

**Step 2: Check for overrun at start of pre-ioctl**

Add at the start of pre-ioctl processing (after `clock_gettime(CLOCK_MONOTONIC, &ioctl_start)`):

```c
/* Check if previous frame overran - if so, consider skipping expensive work */
if (last_frame_total_us > OVERRUN_THRESHOLD_US) {
    consecutive_overruns++;
    if (consecutive_overruns >= SKIP_DSP_THRESHOLD) {
        skip_dsp_this_frame = 1;
        static int skip_log_count = 0;
        if (skip_log_count++ < 10 || skip_log_count % 100 == 0) {
            FILE *f = fopen("/tmp/ioctl_timing.log", "a");
            if (f) {
                fprintf(f, "WARNING: Skipping DSP frame (consecutive overruns: %d, last frame: %llu us)\n",
                        consecutive_overruns, (unsigned long long)last_frame_total_us);
                fclose(f);
            }
        }
    }
} else {
    consecutive_overruns = 0;
    skip_dsp_this_frame = 0;
}
```

**Step 3: Skip DSP when overrunning**

Wrap the DSP rendering call:

```c
#if SHADOW_INPROCESS_POC
shadow_inprocess_handle_ui_request();
shadow_inprocess_handle_param_request();
shadow_inprocess_process_midi();

/* Skip DSP rendering when overrunning to prevent cascading failures */
if (!skip_dsp_this_frame) {
    /* Existing DSP timing code... */
    clock_gettime(CLOCK_MONOTONIC, &dsp_start);
    shadow_inprocess_mix_audio();
    clock_gettime(CLOCK_MONOTONIC, &dsp_end);
    /* ... rest of DSP timing ... */
} else {
    /* When skipping, still need to copy Move's audio through */
    /* shadow_inprocess_mix_audio reads mailbox and writes back -
       if we skip, audio passes through unchanged */
}
#endif
```

**Step 4: Record frame time for next iteration**

At the end of timing calculations:

```c
last_frame_total_us = total_us;
```

**Step 5: Build, deploy, and test**

```bash
./scripts/build.sh && ./scripts/install.sh local
```

Test by playing notes while rapidly changing patches or parameters.

**Step 6: Commit**

```bash
git add src/move_anything_shim.c
git commit -m "feat: add graceful degradation with frame skipping on overrun"
```

---

## Task 3: Investigate Historical Threading Issues

**Files:**
- Read: Git history, any documentation about threading

**Step 1: Search git log for threading references**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything
git log --all --oneline --grep="thread" --grep="audio" --all-match
git log --all --oneline -S "pthread" -- src/move_anything_shim.c
```

**Step 2: Check for comments about threading**

```bash
grep -n -i "thread\|async\|buffer\|latency\|dropout" src/move_anything_shim.c | head -50
```

**Step 3: Document findings**

Create a note about what was tried and why it failed. This informs whether threading is worth revisiting.

---

## Task 4: Optimize Based on Granular Timing Results

This task depends on data from Task 1. The steps will vary based on which section shows the highest max time.

### If `inproc_mix` (DSP) is the culprit:

**Option A: Per-slot timing**
Add timing around each `render_block()` call to identify which plugin is slow:

```c
for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
    if (!shadow_chain_slots[s].active || !shadow_chain_slots[s].instance) continue;

    struct timespec slot_start, slot_end;
    clock_gettime(CLOCK_MONOTONIC, &slot_start);

    shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                   render_buffer, MOVE_FRAMES_PER_BLOCK);

    clock_gettime(CLOCK_MONOTONIC, &slot_end);
    uint64_t slot_us = (slot_end.tv_sec - slot_start.tv_sec) * 1000000 +
                       (slot_end.tv_nsec - slot_start.tv_nsec) / 1000;
    if (slot_us > 50) {  /* Log if any single slot takes >50µs */
        /* Log which slot is slow */
    }
}
```

**Option B: Reduce active slots**
Limit concurrent DSP to 2 slots instead of 4 when under pressure.

### If `ui_req` or `param_req` is the culprit:

These involve plugin API calls that might do file I/O. Options:
- Defer non-critical requests to next frame
- Queue requests and process one per frame
- Cache frequently-accessed params

### If `fwd_midi` is the culprit:

The `access()` syscalls are already cached. If still slow:
- Remove flag file checks entirely, use shared memory flags instead
- Pre-load all filter flags at startup

---

## Task 5: Add DSP Render-Ahead Buffer (Latency Tradeoff)

**Files:**
- Modify: `src/move_anything_shim.c` - `shadow_inprocess_mix_audio()` and ioctl pre-processing

**Concept:** Instead of rendering DSP directly into the mailbox, render 1-2 frames ahead into a ring buffer. This gives us cushion for occasional slow frames.

**Current state:** `shadow_inprocess_mix_audio()` renders and writes directly - zero latency but no tolerance for timing variation.

**Proposed state:** Render into a small ring buffer, read from 1-2 frames behind:

```c
/* Ring buffer for render-ahead */
#define RENDER_AHEAD_FRAMES 2  /* 2 frames = ~5.8ms latency */
static int16_t render_ahead_buffer[RENDER_AHEAD_FRAMES][FRAMES_PER_BLOCK * 2];
static int render_write_idx = 0;
static int render_read_idx = 0;
static int render_buffer_primed = 0;  /* Wait until buffer has data */

static void shadow_inprocess_mix_audio_buffered(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    /* Render into write slot */
    int16_t *render_buf = render_ahead_buffer[render_write_idx];

    /* ... existing render logic writes to render_buf instead of mailbox ... */

    /* Advance write pointer */
    render_write_idx = (render_write_idx + 1) % RENDER_AHEAD_FRAMES;

    /* Prime buffer before starting playback */
    if (!render_buffer_primed) {
        if (render_write_idx >= RENDER_AHEAD_FRAMES - 1) {
            render_buffer_primed = 1;
        }
        return;  /* Don't output until buffer is primed */
    }

    /* Read from oldest slot and mix to mailbox */
    int16_t *read_buf = render_ahead_buffer[render_read_idx];
    render_read_idx = (render_read_idx + 1) % RENDER_AHEAD_FRAMES;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)read_buf[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
}
```

**Latency impact:**
- 1 frame ahead: ~2.9ms added latency
- 2 frames ahead: ~5.8ms added latency (recommended)
- Total latency with 2 frames: Move's existing latency + 5.8ms

**Tradeoff:**
- Pro: Can absorb occasional 2x slow frames without dropout
- Pro: Simpler than threading
- Con: Adds perceptible latency for live playing
- Con: Doesn't help if DSP is consistently slow

**Recommendation:** Start with 1 frame (2.9ms) as a compromise. Only increase if still seeing issues.

---

## Task 6: Consider Double-Buffered Threading (Future)

**Note:** This was allegedly tried before and caused issues. Only pursue after Tasks 1-5 are complete and we understand the current bottleneck.

**Architecture:**
```
Main thread (ioctl):     DSP thread:
┌─────────────────┐     ┌─────────────────┐
│ Copy MIDI to    │     │ Wait for signal │
│ DSP input buf   │────▶│                 │
│                 │     │ Render audio    │
│ Wait for DSP    │◀────│ Signal done     │
│ done (timeout)  │     │                 │
│                 │     │                 │
│ Copy output to  │     │                 │
│ mailbox         │     │                 │
└─────────────────┘     └─────────────────┘
```

**Key considerations:**
- Must not add latency (already tight budget)
- Need lock-free communication (spinlock or atomic)
- If DSP misses deadline, use previous buffer (slight glitch better than crash)
- Thread priority must be real-time

**This is complex and risky. Only attempt if Tasks 1-4 don't solve the problem.**

---

## Verification Checklist

After implementing Tasks 1-4:

1. [ ] Granular timing shows which section causes spikes
2. [ ] Frame skipping activates during overruns (check log)
3. [ ] No crashes during normal playback
4. [ ] No crashes during rapid patch changes
5. [ ] Audio quality acceptable when skipping frames
6. [ ] Move UI still responsive

---

## Files Modified

- `src/move_anything_shim.c` - All timing and mitigation changes
