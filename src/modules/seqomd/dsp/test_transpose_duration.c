/*
 * Test for transpose sequence duration bug
 *
 * Bug: When a long note (e.g., 16 steps) with arp plays through a transpose
 * sequence, all arp notes get the same transpose value (from when the note
 * was triggered), instead of each arp note getting the transpose value for
 * its actual play time.
 *
 * Expected behavior: If transpose sequence has:
 *   - Step 0: transpose=+5, duration=12 (3 beats)
 *   - Step 1: transpose=0, duration=12 (3 beats)
 *
 * And a 16-step arp note starts at step 0, the notes should be:
 *   - Notes at steps 0-11: transposed by +5
 *   - Notes at steps 12-15: transposed by 0
 *
 * Compile: gcc -o test_transpose_duration test_transpose_duration.c -I. -I../../.. -lm
 * Run: ./test_transpose_duration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ============ Test Framework ============ */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Running %s...\n", #name); \
    g_tests_run++; \
    test_##name(); \
    g_tests_passed++; \
    printf("  OK\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAILED at line %d: %s\n", __LINE__, #cond); \
        g_tests_failed++; \
        g_tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  FAILED at line %d: expected %d, got %d\n", __LINE__, (int)(b), (int)(a)); \
        g_tests_failed++; \
        g_tests_passed--; \
        return; \
    } \
} while(0)

/* ============ MIDI Capture ============ */

#define MAX_CAPTURED_NOTES 256

typedef struct {
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;
    int is_note_on;
    double phase;  /* Global phase when captured */
} captured_note_t;

static captured_note_t g_captured_notes[MAX_CAPTURED_NOTES];
static int g_num_captured = 0;

static void clear_captured_notes(void) {
    g_num_captured = 0;
    memset(g_captured_notes, 0, sizeof(g_captured_notes));
}

/* ============ Mock Host API ============ */

static void mock_log(const char *msg) {
    (void)msg;
}

static int mock_midi_send_internal(const uint8_t *msg, int len) {
    (void)msg;
    (void)len;
    return len;
}

/* Forward declare to access g_global_phase */
extern double g_global_phase;

static int mock_midi_send_external(const uint8_t *msg, int len) {
    if (len < 4) return 0;

    uint8_t cin = msg[0] & 0x0F;
    uint8_t status = msg[1];
    uint8_t data1 = msg[2];
    uint8_t data2 = msg[3];

    /* Capture note on/off messages */
    if (cin == 0x9 || cin == 0x8) {
        if (g_num_captured < MAX_CAPTURED_NOTES) {
            captured_note_t *cap = &g_captured_notes[g_num_captured++];
            cap->note = data1;
            cap->velocity = data2;
            cap->channel = status & 0x0F;
            cap->is_note_on = (cin == 0x9 && data2 > 0);
            cap->phase = g_global_phase;
        }
    }

    return len;
}

/* Include plugin API header */
#include "host/plugin_api_v1.h"

static host_api_v1_t g_mock_host;

/* Include plugin source files */
#include "seq_plugin.c"
#include "midi.c"
#include "scheduler.c"
#include "transpose.c"
#include "scale.c"
#include "arpeggiator.c"
#include "track.c"
#include "params.c"

static void init_mock_host(void) {
    g_mock_host.api_version = MOVE_PLUGIN_API_VERSION;
    g_mock_host.sample_rate = MOVE_SAMPLE_RATE;
    g_mock_host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_mock_host.mapped_memory = NULL;
    g_mock_host.audio_out_offset = 0;
    g_mock_host.audio_in_offset = 0;
    g_mock_host.log = mock_log;
    g_mock_host.midi_send_internal = mock_midi_send_internal;
    g_mock_host.midi_send_external = mock_midi_send_external;
}

/* ============ Plugin Interface ============ */

static plugin_api_v1_t *g_plugin = NULL;

static void init_plugin(void) {
    init_mock_host();
    g_plugin = move_plugin_init_v1(&g_mock_host);
    assert(g_plugin != NULL);
    g_plugin->on_load("/test", NULL);
}

static void cleanup_plugin(void) {
    if (g_plugin) {
        g_plugin->on_unload();
        g_plugin = NULL;
    }
}

static void set_param(const char *key, const char *val) {
    g_plugin->set_param(key, val);
}

/* Render enough frames to advance by a given number of steps at current BPM */
static void render_steps(int steps) {
    int bpm = 120;
    double steps_per_second = (double)(bpm * 4) / 60.0;
    int samples_per_step = (int)(MOVE_SAMPLE_RATE / steps_per_second);
    int total_samples = samples_per_step * steps;

    int16_t audio_buf[MOVE_FRAMES_PER_BLOCK * 2];

    while (total_samples > 0) {
        int frames = (total_samples > MOVE_FRAMES_PER_BLOCK) ?
                     MOVE_FRAMES_PER_BLOCK : total_samples;
        g_plugin->render_block(audio_buf, frames);
        total_samples -= frames;
    }
}

/* ============ Tests ============ */

/*
 * Test transpose sequence duration with arp.
 *
 * Setup:
 *   - Transpose sequence: step 0 = +5 for 12 steps, step 1 = 0 for 12 steps
 *   - Track 5 (chord_follow enabled by default) with arp UP at 1/16 speed
 *   - One step with note C4 (60), length = 16 steps
 *
 * Expected:
 *   - 16 arp notes will play (1 per step at 1/16 speed)
 *   - Notes at steps 0-11 should be 60 + 5 = 65
 *   - Notes at steps 12-15 should be 60 + 0 = 60
 */
TEST(transpose_duration_with_arp) {
    init_plugin();
    clear_captured_notes();

    /* Set BPM */
    set_param("bpm", "120");

    /* Set up transpose sequence:
     * Step 0: transpose = +5, duration = 12 (3 beats)
     * Step 1: transpose = 0, duration = 12 (3 beats)
     */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "5");
    set_param("transpose_step_0_duration", "12");
    set_param("transpose_step_1_transpose", "0");
    set_param("transpose_step_1_duration", "12");
    set_param("transpose_step_count", "2");
    set_param("transpose_sequence_enabled", "1");

    /* Track 5 is chord_follow by default, use it */
    int track = 4;  /* 0-indexed, so track 5 = index 4 */

    /* Set up track with arp UP at 1/16 speed (index 2 = 1/16) */
    set_param("track_4_arp_mode", "1");   /* ARP_UP */
    set_param("track_4_arp_speed", "2");  /* 1/16 (1 note per step) */

    /* Add a note on step 0 with length 16 */
    set_param("track_4_step_0_add_note", "60");  /* C4 */
    set_param("track_4_step_0_length", "16");

    /* Start playback */
    set_param("playing", "1");

    /* Render exactly 16 steps to capture notes from the first pattern iteration */
    render_steps(16);

    /* Stop playback */
    set_param("playing", "0");

    /* Analyze captured notes */
    printf("    Captured %d note events\n", g_num_captured);

    /* Count note-ons by pitch */
    int notes_at_65 = 0;  /* Should be 12 (steps 0-11) */
    int notes_at_60 = 0;  /* Should be 4 (steps 12-15) */
    int other_notes = 0;

    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            printf("    Note ON: %d at phase %.2f\n",
                   g_captured_notes[i].note,
                   g_captured_notes[i].phase);
            if (g_captured_notes[i].note == 65) {
                notes_at_65++;
            } else if (g_captured_notes[i].note == 60) {
                notes_at_60++;
            } else {
                other_notes++;
                printf("    UNEXPECTED note: %d\n", g_captured_notes[i].note);
            }
        }
    }

    printf("    Notes at 65 (C4+5): %d (expected 12)\n", notes_at_65);
    printf("    Notes at 60 (C4+0): %d (expected 4)\n", notes_at_60);
    printf("    Other notes: %d (expected 0)\n", other_notes);

    /* Assertions */
    ASSERT_EQ(notes_at_65, 12);  /* First 12 notes should be transposed +5 */
    ASSERT_EQ(notes_at_60, 4);   /* Last 4 notes should be transposed +0 */
    ASSERT_EQ(other_notes, 0);   /* No unexpected notes */

    cleanup_plugin();
}

/*
 * Simpler test: two transpose steps, each 3 beats (12 steps), with a simple
 * non-arp note that spans both.
 *
 * Setup:
 *   - Transpose sequence: step 0 = +5 for 12 steps, step 1 = +2 for 12 steps
 *   - Track 5 with 2x ratchet on a 16-step note (so 2 notes, 8 steps apart)
 *
 * Expected:
 *   - Ratchet note 1 at step 0 should be transposed by +5
 *   - Ratchet note 2 at step 8 should still be +5 (within first transpose step)
 *
 * Actually, let's do a 4x ratchet to make it more interesting:
 *   - Note at step 0: +5
 *   - Note at step 4: +5
 *   - Note at step 8: +5
 *   - Note at step 12: +2 (now in second transpose step!)
 */
TEST(transpose_duration_with_ratchet) {
    init_plugin();
    clear_captured_notes();

    set_param("bpm", "120");

    /* Transpose sequence: 12 steps +5, 12 steps +2 */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "5");
    set_param("transpose_step_0_duration", "12");
    set_param("transpose_step_1_transpose", "2");
    set_param("transpose_step_1_duration", "12");
    set_param("transpose_step_count", "2");
    set_param("transpose_sequence_enabled", "1");

    /* Track 5 (index 4), chord_follow enabled by default */
    /* No arp, but 4x ratchet on a 16-step note */
    set_param("track_4_arp_mode", "0");  /* ARP_OFF */
    set_param("track_4_step_0_add_note", "60");
    set_param("track_4_step_0_length", "16");
    set_param("track_4_step_0_ratchet", "4");  /* 4 hits, one every 4 steps */

    set_param("playing", "1");
    render_steps(16);  /* Exactly one 16-step note */
    set_param("playing", "0");

    /* Count notes */
    int notes_at_65 = 0;  /* 60 + 5 */
    int notes_at_62 = 0;  /* 60 + 2 */

    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            printf("    Ratchet Note ON: %d at phase %.2f\n",
                   g_captured_notes[i].note,
                   g_captured_notes[i].phase);
            if (g_captured_notes[i].note == 65) notes_at_65++;
            else if (g_captured_notes[i].note == 62) notes_at_62++;
        }
    }

    printf("    Notes at 65 (60+5): %d (expected 3: steps 0, 4, 8)\n", notes_at_65);
    printf("    Notes at 62 (60+2): %d (expected 1: step 12)\n", notes_at_62);

    ASSERT_EQ(notes_at_65, 3);  /* Steps 0, 4, 8 are in first transpose region */
    ASSERT_EQ(notes_at_62, 1);  /* Step 12 is in second transpose region */

    cleanup_plugin();
}

/*
 * Test the original user scenario: 3-beat duration steps.
 *
 * User's setup:
 *   - Two trigs (transpose steps), each 3 beats long
 *   - 3 beats = 12 steps (at default speed)
 *   - One arp note that spans 16 steps (4 beats)
 *
 * The user reports that transposition changes on the 4th beat (step 12),
 * but it should change after 3 beats (step 12... which is actually correct!)
 *
 * Wait, the user said "it moves after the fourth beat" - but if the step
 * is 3 beats long, it SHOULD change at beat 4 (step 12).
 *
 * Let me re-read: "if I set three beats it moves after the fourth beat"
 * This suggests the user expects the transpose to change AFTER 3 beats
 * (i.e., at the START of beat 4 = step 12), which IS what should happen.
 *
 * The ACTUAL bug is that all arp notes use the SAME transpose value
 * (the one from step 0), so even after step 12, they still get the
 * wrong transpose.
 */
TEST(user_scenario_3_beat_transpose) {
    init_plugin();
    clear_captured_notes();

    set_param("bpm", "120");

    /* Transpose sequence as user described:
     * Two steps, each 3 beats (12 steps) long
     * Let's say step 0 = +7, step 1 = +12
     */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "7");
    set_param("transpose_step_0_duration", "12");  /* 3 beats */
    set_param("transpose_step_1_transpose", "12");
    set_param("transpose_step_1_duration", "12");  /* 3 beats */
    set_param("transpose_step_count", "2");
    set_param("transpose_sequence_enabled", "1");

    /* Track 5 with arp at 1/16 speed, 16-step note */
    set_param("track_4_arp_mode", "1");   /* ARP_UP */
    set_param("track_4_arp_speed", "2");  /* 1/16 */
    set_param("track_4_step_0_add_note", "60");
    set_param("track_4_step_0_length", "16");

    set_param("playing", "1");
    render_steps(16);  /* Exactly one 16-step note */
    set_param("playing", "0");

    /* Expected:
     * - Steps 0-11 (beats 0-2): 60 + 7 = 67
     * - Steps 12-15 (beats 3): 60 + 12 = 72
     */
    int notes_at_67 = 0;
    int notes_at_72 = 0;

    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            printf("    Note ON: %d at phase %.2f\n",
                   g_captured_notes[i].note,
                   g_captured_notes[i].phase);
            if (g_captured_notes[i].note == 67) notes_at_67++;
            else if (g_captured_notes[i].note == 72) notes_at_72++;
        }
    }

    printf("    Notes at 67 (60+7): %d (expected 12 for steps 0-11)\n", notes_at_67);
    printf("    Notes at 72 (60+12): %d (expected 4 for steps 12-15)\n", notes_at_72);

    ASSERT_EQ(notes_at_67, 12);
    ASSERT_EQ(notes_at_72, 4);

    cleanup_plugin();
}

/* ============ Main ============ */

int main(void) {
    printf("SEQOMD Transpose Duration Tests\n");
    printf("================================\n\n");

    RUN_TEST(transpose_duration_with_arp);
    RUN_TEST(transpose_duration_with_ratchet);
    RUN_TEST(user_scenario_3_beat_transpose);

    printf("\n================================\n");
    printf("Tests: %d run, %d passed, %d failed\n",
           g_tests_run, g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
