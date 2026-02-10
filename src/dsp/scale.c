/*
 * SEQOMD DSP Plugin - Scale Detection
 *
 * Automatic scale detection from chord-follow tracks.
 */

#include "seq_plugin.h"

/**
 * Count set bits in a 16-bit value (popcount).
 */
int popcount16(uint16_t x) {
    int count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

/**
 * Collect all pitch classes from chord-follow tracks.
 * Returns a 12-bit mask where bit N = pitch class N is present.
 * Scans ALL patterns (not just current) to match JS behavior.
 */
uint16_t collect_pitch_classes(void) {
    uint16_t mask = 0;

    for (int t = 0; t < NUM_TRACKS; t++) {
        if (!g_chord_follow[t]) continue;

        /* Scan all patterns for this track */
        for (int p = 0; p < NUM_PATTERNS; p++) {
            pattern_t *pattern = &g_tracks[t].patterns[p];
            for (int s = 0; s < NUM_STEPS; s++) {
                step_t *step = &pattern->steps[s];
                for (int n = 0; n < step->num_notes; n++) {
                    if (step->notes[n] > 0) {
                        int pitch_class = step->notes[n] % 12;
                        mask |= (1 << pitch_class);
                    }
                }
            }
        }
    }

    return mask;
}

/**
 * Score how well pitch classes fit a scale template at a given root.
 * Returns score * 1000 for integer comparison (higher = better).
 */
int score_scale(uint16_t pitch_mask, int scale_idx, int root) {
    if (pitch_mask == 0) return 0;

    /* Build scale mask for this root */
    uint16_t scale_mask = 0;
    for (int i = 0; i < g_scale_templates[scale_idx].note_count; i++) {
        int pc = (g_scale_templates[scale_idx].notes[i] + root) % 12;
        scale_mask |= (1 << pc);
    }

    /* Count notes in scale */
    int in_scale = popcount16(pitch_mask & scale_mask);
    int total = popcount16(pitch_mask);

    if (total == 0) return 0;

    /* Score: fit ratio * 1000 + small bonus for simpler scales */
    int fit_score = (in_scale * 1000) / total;
    int size_bonus = 100 / g_scale_templates[scale_idx].note_count;

    return fit_score + size_bonus;
}

/**
 * Detect the best-fitting scale from chord-follow track notes.
 * Updates g_detected_scale_root and g_detected_scale_index.
 */
void detect_scale(void) {
    uint16_t pitch_mask = collect_pitch_classes();

    if (pitch_mask == 0) {
        g_detected_scale_root = -1;
        g_detected_scale_index = -1;
        g_scale_dirty = 0;
        return;
    }

    int best_score = -1;
    int best_root = 0;
    int best_scale = 0;

    for (int root = 0; root < 12; root++) {
        for (int scale = 0; scale < NUM_SCALE_TEMPLATES; scale++) {
            int score = score_scale(pitch_mask, scale, root);
            if (score > best_score) {
                best_score = score;
                best_root = root;
                best_scale = scale;
            }
        }
    }

    g_detected_scale_root = best_root;
    g_detected_scale_index = best_scale;
    g_scale_dirty = 0;
}
