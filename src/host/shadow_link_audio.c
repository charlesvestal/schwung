/* shadow_link_audio.c — Link Audio read helper + minimal shared state.
 *
 * Post-migration (2026-04), Move audio is received by the link-subscriber
 * sidecar and delivered via /schwung-link-in. This file used to host the
 * legacy chnnlsv sendto() hook parser and an in-process publisher thread;
 * both are gone. What remains is the consumer-side SHM reader and the
 * per-slot capture buffer used by the publisher SHM writer in schwung_shim.c.
 */

#include <string.h>
#include "shadow_link_audio.h"

/* ============================================================================
 * Globals
 * ============================================================================ */

link_audio_state_t link_audio;

/* Per-slot captured audio (written by render code, read by /schwung-pub-audio
 * writer in schwung_shim.c). */
int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

/* ============================================================================
 * Init / reset
 * ============================================================================ */

void shadow_link_audio_init(void) {
    memset(&link_audio, 0, sizeof(link_audio));
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));
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

    if (avail < need) {
        __atomic_fetch_add(&slot->starve_count, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* Hard catch-up: if producer got ahead by more than 12 blocks (~70 ms),
     * jump to the most recent block. Burst-protection only — kicks in when
     * the SDK thread paused and flushed a long backlog. Soft slip below
     * handles steady-state rate drift without an audible cliff. */
    if (avail > need * 12) {
        uint32_t new_rp = wp - need;
        __atomic_fetch_add(&slot->catchup_samples_dropped,
                           (new_rp - rp), __ATOMIC_RELAXED);
        __atomic_fetch_add(&slot->catchup_count, 1, __ATOMIC_RELAXED);
        rp = new_rp;
    }

    /* Soft slip: when producer rate slightly exceeds consumer rate the ring
     * fills steadily. Without intervention it grows until the hard catch-up
     * fires and dumps ~70 ms in one go (audible glitch). Instead, when avail
     * stays above target for several consecutive reads, drop one stereo
     * frame per call — 1/128 sample ≈ 0.78% rate adjustment, inaudible.
     * Per-slot static is fine: SPSC, single reader thread per slot.
     *
     * Target: 2 blocks of headroom (need*2). Anything above means we're
     * accumulating; below, we're at or near steady state. */
    static int slip_streak[LINK_AUDIO_IN_SLOT_COUNT] = {0};
    const uint32_t slip_target = need * 2;
    if (avail > slip_target) {
        if (++slip_streak[slot_idx] >= 8) {
            slip_streak[slot_idx] = 0;
            rp += 2;  /* drop one stereo frame */
            __atomic_fetch_add(&slot->catchup_samples_dropped, 2,
                               __ATOMIC_RELAXED);
        }
    } else {
        slip_streak[slot_idx] = 0;
    }

    for (uint32_t i = 0; i < need; i++) {
        out_lr[i] = slot->ring[(rp + i) & LINK_AUDIO_IN_RING_MASK];
    }

    __sync_synchronize();
    slot->read_pos = rp + need;
    return 1;
}
