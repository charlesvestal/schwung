/*
 * SEQOMD DSP Plugin - Arpeggiator
 *
 * Arpeggiator pattern generation.
 */

#include "seq_plugin.h"

/**
 * Get the pattern length from play_steps value (highest set bit position + 1).
 * @param play_steps Value 1-255
 * @return Pattern length in bits (1-8)
 */
int get_play_steps_length(uint8_t play_steps) {
    if (play_steps == 0) return 1;
    int len = 0;
    while (play_steps > 0) {
        len++;
        play_steps >>= 1;
    }
    return len;
}

/* Helper: sort notes by pitch (insertion sort, small arrays) */
void sort_notes(uint8_t *notes, int count) {
    for (int i = 1; i < count; i++) {
        uint8_t key = notes[i];
        int j = i - 1;
        while (j >= 0 && notes[j] > key) {
            notes[j + 1] = notes[j];
            j--;
        }
        notes[j + 1] = key;
    }
}

/* Helper: shuffle array (Fisher-Yates) */
void shuffle_notes(uint8_t *notes, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = random_check(100) * i / 100;  /* Simple random index */
        if (j > i) j = i;
        uint8_t tmp = notes[i];
        notes[i] = notes[j];
        notes[j] = tmp;
    }
}

/**
 * Generate arp pattern for given notes.
 * Notes are sorted by pitch, then arranged according to arp_mode.
 * Octave extension is applied if arp_octave > 0.
 * Returns pattern length.
 */
int generate_arp_pattern(uint8_t *notes, int num_notes, int arp_mode,
                         int arp_octave, uint8_t *out_pattern, int max_len) {
    if (num_notes <= 0 || num_notes > MAX_NOTES_PER_STEP) return 0;

    /* Copy and sort notes by pitch */
    uint8_t sorted[MAX_NOTES_PER_STEP];
    for (int i = 0; i < num_notes; i++) {
        sorted[i] = notes[i];
    }
    sort_notes(sorted, num_notes);

    int len = 0;

    /* Generate base pattern based on mode */
    switch (arp_mode) {
        case ARP_UP:
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN:
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_UP_DOWN:
            /* Up then down, includes endpoints twice: C-E-G-E */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = num_notes - 2; i > 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN_UP:
            /* Down then up, includes endpoints twice: G-E-C-E */
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = 1; i < num_notes - 1 && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_UP_AND_DOWN:
            /* Up then down, no repeated endpoints: C-E-G-G-E-C */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_DOWN_AND_UP:
            /* Down then up, no repeated endpoints: G-E-C-C-E-G */
            for (int i = num_notes - 1; i >= 0 && len < max_len; i--) {
                out_pattern[len++] = sorted[i];
            }
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_RANDOM:
            /* Shuffle the sorted notes */
            for (int i = 0; i < num_notes; i++) {
                out_pattern[i] = sorted[i];
            }
            shuffle_notes(out_pattern, num_notes);
            len = num_notes;
            break;

        case ARP_CHORD:
            /* All notes at once - just return first note, caller handles chord */
            /* For scheduling purposes, we treat this as playing all notes at each position */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_OUTSIDE_IN:
            /* High/low alternating inward: G-C-E (for C-E-G) */
            for (int i = 0; i < (num_notes + 1) / 2 && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[num_notes - 1 - i];
                if (i != num_notes - 1 - i && len < max_len) {
                    out_pattern[len++] = sorted[i];
                }
            }
            break;

        case ARP_INSIDE_OUT:
            /* Middle outward alternating */
            {
                int mid = num_notes / 2;
                if (len < max_len) out_pattern[len++] = sorted[mid];
                for (int i = 1; i <= mid && len < max_len; i++) {
                    if (mid + i < num_notes && len < max_len) {
                        out_pattern[len++] = sorted[mid + i];
                    }
                    if (mid - i >= 0 && len < max_len) {
                        out_pattern[len++] = sorted[mid - i];
                    }
                }
            }
            break;

        case ARP_CONVERGE:
            /* Low/high pairs moving in: C-G-E (for C-E-G) */
            for (int i = 0; i < (num_notes + 1) / 2 && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[i];
                if (i != num_notes - 1 - i && len < max_len) {
                    out_pattern[len++] = sorted[num_notes - 1 - i];
                }
            }
            break;

        case ARP_DIVERGE:
            /* Middle expanding out (same as inside_out for notes) */
            {
                int mid = num_notes / 2;
                if (len < max_len) out_pattern[len++] = sorted[mid];
                for (int i = 1; i <= mid && len < max_len; i++) {
                    if (mid + i < num_notes && len < max_len) {
                        out_pattern[len++] = sorted[mid + i];
                    }
                    if (mid - i >= 0 && len < max_len) {
                        out_pattern[len++] = sorted[mid - i];
                    }
                }
            }
            break;

        case ARP_THUMB:
            /* Bass note pedal: C-C-E-C-G */
            if (len < max_len) out_pattern[len++] = sorted[0];
            for (int i = 1; i < num_notes && len < max_len; i++) {
                if (len < max_len) out_pattern[len++] = sorted[0];
                if (len < max_len) out_pattern[len++] = sorted[i];
            }
            break;

        case ARP_PINKY:
            /* Top note pedal: G-G-E-G-C */
            if (len < max_len) out_pattern[len++] = sorted[num_notes - 1];
            for (int i = num_notes - 2; i >= 0 && len < max_len; i--) {
                if (len < max_len) out_pattern[len++] = sorted[num_notes - 1];
                if (len < max_len) out_pattern[len++] = sorted[i];
            }
            break;

        default:
            /* Default to up */
            for (int i = 0; i < num_notes && len < max_len; i++) {
                out_pattern[len++] = sorted[i];
            }
            break;
    }

    /* Apply octave extension */
    if (arp_octave != ARP_OCT_NONE && len > 0) {
        int base_len = len;
        uint8_t base_pattern[MAX_ARP_PATTERN];
        for (int i = 0; i < base_len; i++) {
            base_pattern[i] = out_pattern[i];
        }

        len = 0;  /* Reset and rebuild with octaves */

        switch (arp_octave) {
            case ARP_OCT_UP1:
                /* Base, then +12 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_UP2:
                /* Base, +12, +24 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 24;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_DOWN1:
                /* -12, then base */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                break;

            case ARP_OCT_DOWN2:
                /* -24, -12, base */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 24;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                break;

            case ARP_OCT_BOTH1:
                /* -12, base, +12 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;

            case ARP_OCT_BOTH2:
                /* -24, -12, base, +12, +24 */
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 24;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] - 12;
                    if (note >= 0) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    out_pattern[len++] = base_pattern[i];
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 12;
                    if (note <= 127) out_pattern[len++] = note;
                }
                for (int i = 0; i < base_len && len < max_len; i++) {
                    int note = base_pattern[i] + 24;
                    if (note <= 127) out_pattern[len++] = note;
                }
                break;
        }
    }

    return len;
}
