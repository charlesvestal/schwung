/* shadow_link_audio.c — Link Audio read helper + minimal shared state.
 *
 * Post-migration (2026-04), Move audio is received by the link-subscriber
 * sidecar and delivered via /schwung-link-in. This file used to host the
 * legacy chnnlsv sendto() hook parser and an in-process publisher thread;
 * both are gone. What remains is the consumer-side SHM reader and the
 * per-slot capture buffer used by the publisher SHM writer in schwung_shim.c.
 */

#include <stdint.h>
#include <string.h>
#include "shadow_link_audio.h"
#include "shadow_resample.h"  /* latency_comp_active */

/* ============================================================================
 * Globals
 * ============================================================================ */

link_audio_state_t link_audio;

/* Per-slot captured audio (written by render code, read by /schwung-pub-audio
 * writer in schwung_shim.c). */
int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

/* Per-slot `avail` accumulators for latency profiling. Sole writer is the
 * SPI thread inside link_audio_read_channel_shm; sole reader/resetter is
 * the background timing logger via link_audio_drain_avail_stats. Not in
 * SHM — purely shim-internal, no sidecar coordination needed. */
static volatile uint32_t la_avail_min[LINK_AUDIO_IN_SLOT_COUNT];
static volatile uint32_t la_avail_max[LINK_AUDIO_IN_SLOT_COUNT];
static volatile uint64_t la_avail_sum[LINK_AUDIO_IN_SLOT_COUNT];
static volatile uint32_t la_avail_count[LINK_AUDIO_IN_SLOT_COUNT];

static void la_avail_stats_reset(void) {
    for (int i = 0; i < LINK_AUDIO_IN_SLOT_COUNT; i++) {
        la_avail_min[i]   = UINT32_MAX;
        la_avail_max[i]   = 0;
        la_avail_sum[i]   = 0;
        la_avail_count[i] = 0;
    }
}

/* Per-slot nudge counters for adaptive ring-fill correction. Rate-limit the
 * sample drop/duplicate operation to once every NUDGE_PERIOD reads, which
 * at ~344 reads/sec works out to ~0.3% rate change — still well below the
 * click threshold (one 22.7 µs sample skip every 47 ms) and below pitch-
 * shift audibility for transient program material. Dead band of ±256
 * samples (~2.9 ms) prevents jittering around the target. Convergence from
 * a 5 ms-off plateau takes ~5 s. Burst mode (drop/dup 8 frames per period
 * instead of 1) kicks in when error exceeds NUDGE_BURST_THRESHOLD so the
 * first second after engage gets us most of the way to target. */
#define NUDGE_DEAD_BAND_SAMPLES 32
#define NUDGE_PERIOD            16
#define NUDGE_BURST_THRESHOLD   512
#define NUDGE_BURST_FRAMES      8
static uint32_t nudge_drop_counter[LINK_AUDIO_IN_SLOT_COUNT];
static uint32_t nudge_dup_counter[LINK_AUDIO_IN_SLOT_COUNT];

void link_audio_reset_nudge_state(void) {
    for (int i = 0; i < LINK_AUDIO_IN_SLOT_COUNT; i++) {
        nudge_drop_counter[i] = 0;
        nudge_dup_counter[i]  = 0;
    }
}

/* ============================================================================
 * Init / reset
 * ============================================================================ */

void shadow_link_audio_init(void) {
    memset(&link_audio, 0, sizeof(link_audio));
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));
    la_avail_stats_reset();
    link_audio_reset_nudge_state();
}

void link_audio_reset_state(void) {
    /* Post-migration this is just a no-op safety net — the sidecar owns
     * reception, so there is no in-process channel state to clear.
     * Called from shadow_process.c during link-subscriber restart. */
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));
}

/* ============================================================================
 * Consumer-side SHM reader
 * ============================================================================ */

int link_audio_read_channel_shm(link_audio_in_shm_t *shm, int slot_idx,
                                int16_t *out_lr, int frames) {
    if (!shm || !out_lr || frames <= 0) return 0;
    if (slot_idx < 0 || slot_idx >= LINK_AUDIO_IN_SLOT_COUNT) return 0;

    link_audio_in_slot_t *slot = &shm->slots[slot_idx];

    __sync_synchronize();
    if (!slot->active) return 0;

    uint32_t wp = slot->write_pos;
    uint32_t rp = slot->read_pos;  /* we are the sole consumer */
    uint32_t avail = wp - rp;       /* wraps correctly on unsigned overflow */
    uint32_t need = (uint32_t)(frames * 2);

    /* Telemetry RMWs use relaxed atomics. Plain RMWs raced the background
     * logger's read+reset and undercounted drops. The values are purely
     * informational so RELAXED ordering is enough. */
    uint32_t prev_max = __atomic_load_n(&slot->max_avail_seen,
                                        __ATOMIC_RELAXED);
    if (avail > prev_max) {
        __atomic_store_n(&slot->max_avail_seen, avail, __ATOMIC_RELAXED);
    }

    /* Shim-local avail profiling (min/max/sum/count). Sampled pre-catch-up
     * because the catch-up jump rewrites rp and would mask the actual
     * pending-latency value we want to characterize. */
    {
        uint32_t cur_min = __atomic_load_n(&la_avail_min[slot_idx],
                                           __ATOMIC_RELAXED);
        if (avail < cur_min) {
            __atomic_store_n(&la_avail_min[slot_idx], avail, __ATOMIC_RELAXED);
        }
        uint32_t cur_max = __atomic_load_n(&la_avail_max[slot_idx],
                                           __ATOMIC_RELAXED);
        if (avail > cur_max) {
            __atomic_store_n(&la_avail_max[slot_idx], avail, __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(&la_avail_sum[slot_idx], avail, __ATOMIC_RELAXED);
        __atomic_fetch_add(&la_avail_count[slot_idx], 1, __ATOMIC_RELAXED);
    }

    if (avail < need) {
        __atomic_fetch_add(&slot->starve_count, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* Catch-up: if producer got ahead by more than 12 blocks (~70 ms), jump
     * to the most recent block. Every catch-up is an audible drop, so the
     * threshold has to tolerate real producer-thread jitter — the Link audio
     * SDK thread can pause ~25 ms and then flush buffered audio. Raised from
     * need*4 (1024 samples) to need*12 (3072) after instrumentation showed
     * observed bursts were consistently <30 ms. If true long-term drift
     * exists, max_avail_seen will rise steadily and surface in the logger. */
    if (avail > need * 12) {
        uint32_t new_rp = wp - need;
        __atomic_fetch_add(&slot->catchup_samples_dropped,
                           (new_rp - rp), __ATOMIC_RELAXED);
        __atomic_fetch_add(&slot->catchup_count, 1, __ATOMIC_RELAXED);
        rp = new_rp;
    }

    /* Adaptive nudge toward LATENCY_COMP_TARGET_SAMPLES. Drops or duplicates
     * stereo frames every NUDGE_PERIOD reads when outside the dead band.
     * Burst mode (NUDGE_BURST_FRAMES at once) engages on first second after
     * activate so we converge fast; otherwise single-frame steps. Only
     * runs when latency_comp_active is latched (set on rebuild_from_la
     * engage transition by the shim mixer). */
    int delta_frames = 0;
    /* delta_frames > 0 → duplicate that many frames; < 0 → drop that many */
    if (latency_comp_active) {
        uint32_t target = LATENCY_COMP_TARGET_SAMPLES;
        if (avail > target + NUDGE_DEAD_BAND_SAMPLES) {
            if (++nudge_drop_counter[slot_idx] >= NUDGE_PERIOD) {
                nudge_drop_counter[slot_idx] = 0;
                uint32_t error = avail - target;
                int frames = (error > NUDGE_BURST_THRESHOLD)
                             ? NUDGE_BURST_FRAMES : 1;
                /* Cap drop so we don't shrink the read below the ring */
                uint32_t skip_samples = (uint32_t)frames * 2;
                if (avail >= need + skip_samples) {
                    delta_frames = -frames;
                    rp += skip_samples;
                }
            }
            nudge_dup_counter[slot_idx] = 0;
        } else if (avail + NUDGE_DEAD_BAND_SAMPLES < target) {
            if (++nudge_dup_counter[slot_idx] >= NUDGE_PERIOD) {
                nudge_dup_counter[slot_idx] = 0;
                uint32_t error = target - avail;
                int frames = (error > NUDGE_BURST_THRESHOLD)
                             ? NUDGE_BURST_FRAMES : 1;
                /* Need to keep at least 2 source samples in the read */
                if (need > (uint32_t)frames * 2 + 2) {
                    delta_frames = +frames;
                }
            }
            nudge_drop_counter[slot_idx] = 0;
        } else {
            nudge_drop_counter[slot_idx] = 0;
            nudge_dup_counter[slot_idx]  = 0;
        }
    } else {
        nudge_drop_counter[slot_idx] = 0;
        nudge_dup_counter[slot_idx]  = 0;
    }

    if (delta_frames > 0) {
        /* Duplicate `delta_frames` frames: read (need - delta*2) samples,
         * then repeat the last L/R pair to pad out. */
        uint32_t pad_samples = (uint32_t)delta_frames * 2;
        uint32_t read_n = need - pad_samples;
        for (uint32_t i = 0; i < read_n; i++) {
            out_lr[i] = slot->ring[(rp + i) & LINK_AUDIO_IN_RING_MASK];
        }
        int16_t last_l = out_lr[read_n - 2];
        int16_t last_r = out_lr[read_n - 1];
        for (uint32_t i = read_n; i < need; i += 2) {
            out_lr[i]     = last_l;
            out_lr[i + 1] = last_r;
        }
        __sync_synchronize();
        slot->read_pos = rp + read_n;
    } else {
        for (uint32_t i = 0; i < need; i++) {
            out_lr[i] = slot->ring[(rp + i) & LINK_AUDIO_IN_RING_MASK];
        }
        __sync_synchronize();
        slot->read_pos = rp + need;
    }
    return 1;
}

void link_audio_drain_avail_stats(int slot_idx,
                                  uint32_t *out_min,
                                  uint32_t *out_max,
                                  uint64_t *out_sum,
                                  uint32_t *out_count)
{
    if (slot_idx < 0 || slot_idx >= LINK_AUDIO_IN_SLOT_COUNT) {
        if (out_min)   *out_min = 0;
        if (out_max)   *out_max = 0;
        if (out_sum)   *out_sum = 0;
        if (out_count) *out_count = 0;
        return;
    }
    uint32_t mn = __atomic_exchange_n(&la_avail_min[slot_idx],
                                      UINT32_MAX, __ATOMIC_RELAXED);
    uint32_t mx = __atomic_exchange_n(&la_avail_max[slot_idx],
                                      0, __ATOMIC_RELAXED);
    uint64_t sm = __atomic_exchange_n(&la_avail_sum[slot_idx],
                                      0, __ATOMIC_RELAXED);
    uint32_t ct = __atomic_exchange_n(&la_avail_count[slot_idx],
                                      0, __ATOMIC_RELAXED);
    if (out_min)   *out_min = (ct == 0) ? 0 : mn;
    if (out_max)   *out_max = mx;
    if (out_sum)   *out_sum = sm;
    if (out_count) *out_count = ct;
}
