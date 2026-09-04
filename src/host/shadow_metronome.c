/* See shadow_metronome.h. Everything here runs on the SPI callback. */

#include "shadow_metronome.h"
#include "metronome_click.h"

#define METRONOME_SAMPLE_RATE 44100.0f

/*
 * Full-scale amplitude at level 100. -12 dBFS: the click sits over a full mix
 * without being the loudest thing in it, and leaves headroom so a click landing
 * on a peak cannot be what clips the mailbox.
 */
#define METRONOME_FULL_SCALE 8192.0f

static metronome_voice_t g_voice;
static int g_last_pulses = -1;

/* Frames until the scheduled click sounds, and which beat it is. Negative =
 * nothing scheduled. One slot only: the delay is 15.9 ms and the shortest
 * musically reachable beat is 200 ms at 300 BPM, so two cannot be in flight —
 * and if one ever were, the later boundary replacing the earlier is the right
 * answer anyway. */
static int g_pending_frames = -1;
static int g_pending_beat = 0;

void shadow_metronome_reset(void)
{
    g_voice.amp = 0.0f;
    g_last_pulses = -1;
    g_pending_frames = -1;
}

int shadow_metronome_render(int16_t *out_lr, int frames,
                            int mode, int move_on, int playing,
                            int pulses, int beats_per_bar, int level_pct)
{
    if (!out_lr || frames <= 0) return 0;

    if (mode == SHADOW_METRONOME_OFF ||
        (mode == SHADOW_METRONOME_FOLLOW && !move_on)) {
        shadow_metronome_reset();
        return 0;
    }

    /* A queue with no clock never fires. Stopped means silent, and it also
     * means FORGETTING where we were: the next Start zeroes the pulse count,
     * and a stale g_last_pulses would swallow that first downbeat — the one
     * click of a take that matters most. */
    if (!playing) {
        shadow_metronome_reset();
        return 0;
    }

    if (beats_per_bar <= 0) beats_per_bar = METRONOME_DEFAULT_BEATS_PER_BAR;

    /* g_last_pulses < 0 is "no prior position", so there is nothing to have
     * crossed. metronome_beat_crossed() guards this too; the guard is here as
     * well because it is what makes switching the metronome on mid-bar quiet
     * until the next beat rather than clicking immediately. */
    if (g_last_pulses >= 0) {
        int beat = metronome_beat_crossed(g_last_pulses, pulses, beats_per_bar);
        if (beat >= 0) {
            /* SCHEDULED, not sounded. The boundary is "now" on Move's clock,
             * but Move's audio in this same block came through Link Audio and
             * is a transit older — so a click sounded here leads the music.
             * Hold it back to meet the audio it is a reference for. */
            g_pending_frames = METRONOME_LA_COMP_FRAMES;
            g_pending_beat = beat;
        }
    }
    g_last_pulses = pulses;

    if (level_pct < 0) level_pct = 0;
    if (level_pct > 100) level_pct = 100;

    int start_at = metronome_pending_advance(&g_pending_frames, frames);

    /* The countdown still advances at level 0 — the schedule is state, not
     * output — but nothing is triggered, so the block is genuinely untouched
     * and must be REPORTED as untouched. Triggering a zero-amplitude voice and
     * returning 1 would claim audio we did not add, which is the same defect
     * class as a write that discards and reports success. */
    if (level_pct == 0) start_at = -1;

    if (start_at < 0 && !metronome_voice_active(&g_voice)) return 0;

    for (int i = 0; i < frames; i++) {
        if (i == start_at) {
            metronome_voice_trigger(&g_voice,
                                    g_pending_beat == 0 ? METRONOME_FREQ_DOWNBEAT_HZ
                                                        : METRONOME_FREQ_BEAT_HZ,
                                    (float)level_pct / 100.0f,
                                    METRONOME_DECAY_SECONDS,
                                    METRONOME_SAMPLE_RATE);
        }
        float s = metronome_voice_next(&g_voice) * METRONOME_FULL_SCALE;
        for (int ch = 0; ch < 2; ch++) {
            int idx = i * 2 + ch;
            int32_t mixed = (int32_t)out_lr[idx] + (int32_t)s;
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out_lr[idx] = (int16_t)mixed;
        }
    }
    return 1;
}
