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

    if (avail < need) return 0;

    /* Catch-up: if producer got ahead by more than 4 blocks, jump to the most
     * recent block. Mirrors the legacy link_audio_read_channel() behavior —
     * prevents unbounded latency drift when producer/consumer clocks differ. */
    if (avail > need * 4) {
        rp = wp - need;
    }

    for (uint32_t i = 0; i < need; i++) {
        out_lr[i] = slot->ring[(rp + i) & LINK_AUDIO_IN_RING_MASK];
    }

    __sync_synchronize();
    slot->read_pos = rp + need;
    return 1;
}
