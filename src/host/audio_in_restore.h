#ifndef AUDIO_IN_RESTORE_H
#define AUDIO_IN_RESTORE_H

/*
 * Should the post-ioctl restore re-copy hardware AUDIO_IN over the shadow
 * mailbox's copy?
 *
 * Two writers want the same 512 bytes at offset 2304, in this order inside
 * one shim_post_transfer:
 *
 *   1. native_resample_bridge_apply()  writes Schwung's mix there, so Move's
 *      own Resample records what Schwung is playing.
 *   2. the restore in shadow_inprocess_render_to_buffer() writes the jack
 *      there, so an overtake plugin reading host->audio_in_offset gets the
 *      actual input rather than the bridge's leftovers.
 *
 * The restore runs second, so unguarded it wins every frame and the bridge
 * silently does nothing for as long as an overtake module is loaded — a
 * resample that captures the jack instead of the mix, with nothing logged.
 * The bridge is the deliberate, opt-in, user-visible setting (default OFF),
 * so it takes precedence; the restore stands down while it is applying.
 *
 * Pure so tests/host can drive the four-way table.
 */
static inline int shadow_audio_in_restore_allowed(int overtake_inst_present,
                                                  int hardware_mmap_present,
                                                  int bridge_mode_on,
                                                  int bridge_source_allows)
{
    if (!overtake_inst_present || !hardware_mmap_present) return 0;
    if (bridge_mode_on && bridge_source_allows) return 0;
    return 1;
}

#endif /* AUDIO_IN_RESTORE_H */
