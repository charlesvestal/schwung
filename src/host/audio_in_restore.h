#ifndef AUDIO_IN_RESTORE_H
#define AUDIO_IN_RESTORE_H

/*
 * Should the post-ioctl restore re-copy hardware AUDIO_IN over the shadow
 * mailbox's copy, so an overtake plugin reading host->audio_in_offset gets
 * the jack?
 *
 * There used to be a third argument, "is the resample bridge on", and the
 * restore stood down while it was (#457): the bridge wrote Schwung's mix into
 * the region EARLIER in the same pass, and a restore after it silently undid
 * it. The bridge now runs LAST in shim_post_transfer, after the render, so
 * nothing can undo it and every reader in the render -- a chain Line In slot
 * as much as an overtake plugin -- sees the jack. Keeping the argument would
 * have kept a stand-down whose only remaining effect is to hand an overtake
 * plugin the previous frame's mix instead of its input.
 *
 * Pure so tests/host can drive the table.
 */
static inline int shadow_audio_in_restore_allowed(int overtake_inst_present,
                                                  int hardware_mmap_present)
{
    if (!overtake_inst_present || !hardware_mmap_present) return 0;
    return 1;
}

#endif /* AUDIO_IN_RESTORE_H */
