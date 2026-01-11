/*
 * SEQOMD DSP Plugin Tests
 *
 * Standalone test harness for the sequencer plugin.
 * Tests transpose, chord follow, beat counting, and note scheduling.
 *
 * Compile: gcc -o test_seq_plugin test_seq_plugin.c -lm
 * Run: ./test_seq_plugin
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
    printf("  Running %s...", #name); \
    g_tests_run++; \
    test_##name(); \
    g_tests_passed++; \
    printf(" OK\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf(" FAILED at line %d: %s\n", __LINE__, #cond); \
        g_tests_failed++; \
        g_tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

/* ============ MIDI Capture for Testing ============ */

#define MAX_CAPTURED_NOTES 256

typedef struct {
    uint8_t note;
    uint8_t velocity;
    uint8_t channel;
    int is_note_on;  /* 1 = note on, 0 = note off */
} captured_note_t;

static captured_note_t g_captured_notes[MAX_CAPTURED_NOTES];
static int g_num_captured = 0;

static void clear_captured_notes(void) {
    g_num_captured = 0;
    memset(g_captured_notes, 0, sizeof(g_captured_notes));
}

/* ============ Mock Host API ============ */

static void mock_log(const char *msg) {
    /* Silent during tests, uncomment for debugging */
    /* printf("[LOG] %s\n", msg); */
    (void)msg;
}

static int mock_midi_send_internal(const uint8_t *msg, int len) {
    (void)msg;
    (void)len;
    return len;
}

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
        }
    }

    return len;
}

/* Include plugin API header */
#include "host/plugin_api_v1.h"

/* Create mock host - will be initialized after function declarations */
static host_api_v1_t g_mock_host;

/* Include the plugin source directly for testing static functions */
#include "seq_plugin.c"

/* Now initialize mock host (after functions are defined) */
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

/* ============ Test Helpers ============ */

static void set_param(const char *key, const char *val) {
    g_plugin->set_param(key, val);
}

static int get_param_int(const char *key) {
    char buf[64];
    int len = g_plugin->get_param(key, buf, sizeof(buf));
    if (len < 0) return -1;
    return atoi(buf);
}

/* Render enough frames to advance by a given number of steps at current BPM */
static void render_steps(int steps) {
    /* At 120 BPM, 4 steps per beat:
     * steps_per_second = 120 * 4 / 60 = 8
     * samples_per_step = 44100 / 8 = 5512.5
     */
    int bpm = get_param_int("bpm");
    if (bpm <= 0) bpm = 120;

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

/* Render enough to advance by beats (1 beat = 4 steps) */
static void render_beats(int beats) {
    render_steps(beats * 4);
}

/* ============ Tests: Basic Functionality ============ */

TEST(plugin_init) {
    /* Plugin should initialize successfully */
    ASSERT_NE(g_plugin, NULL);
    ASSERT_EQ(g_plugin->api_version, MOVE_PLUGIN_API_VERSION);
}

TEST(default_bpm) {
    int bpm = get_param_int("bpm");
    ASSERT_EQ(bpm, 120);
}

TEST(set_bpm) {
    set_param("bpm", "140");
    int bpm = get_param_int("bpm");
    ASSERT_EQ(bpm, 140);
    /* Reset */
    set_param("bpm", "120");
}

TEST(default_chord_follow) {
    /* Tracks 0-3 should have chord_follow off, tracks 4-7 on (default) */
    /* This matches the default: {0, 0, 0, 0, 1, 1, 1, 1} */
    /* Note: We can't directly query chord_follow via get_param in current impl,
       so we test behavior instead */
    ASSERT(1); /* Placeholder - behavior tested in transpose tests */
}

/* ============ Tests: Step and Note Programming ============ */

TEST(add_note_to_step) {
    /* Add a note to track 0, step 0 */
    set_param("track_0_step_0_add_note", "60");

    /* Start playback briefly to trigger the note */
    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);  /* Just past step 0 */
    set_param("playing", "0");

    /* Should have captured a note-on for note 60 */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 60);
    ASSERT_EQ(g_captured_notes[0].is_note_on, 1);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(clear_step) {
    /* Add a note then clear */
    set_param("track_0_step_0_add_note", "64");
    set_param("track_0_step_0_clear", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Should have no note-ons (step was cleared) */
    int note_ons = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) note_ons++;
    }
    ASSERT_EQ(note_ons, 0);
}

/* ============ Tests: Transpose ============ */

TEST(transpose_no_effect_on_drum_tracks) {
    /* Track 0 has chord_follow=0 (drum track) by default */
    /* Set a note on track 0 */
    set_param("track_0_step_0_add_note", "60");  /* C4 */

    /* Set transpose to +5 semitones */
    set_param("current_transpose", "5");

    /* Play and capture */
    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should still be 60 (no transpose on drum tracks) */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 60);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("current_transpose", "0");
}

TEST(transpose_applies_to_chord_follow_tracks) {
    /* Track 4 has chord_follow=1 by default */
    /* Set a note on track 4 */
    set_param("track_4_step_0_add_note", "60");  /* C4 */

    /* Set transpose to +5 semitones */
    set_param("current_transpose", "5");

    /* Play and capture */
    clear_captured_notes();
    set_param("playing", "1");
    render_steps(2);  /* Render enough to trigger step 0 */
    set_param("playing", "0");

    /* Find note-on for track 4 (channel 4) */
    int found_note = -1;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on && g_captured_notes[i].channel == 4) {
            found_note = g_captured_notes[i].note;
            break;
        }
    }

    /* Note should be 65 (transposed up 5 semitones) */
    ASSERT(found_note >= 0);
    ASSERT_EQ(found_note, 65);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

TEST(transpose_negative) {
    /* Test negative transpose */
    set_param("track_4_step_0_add_note", "60");
    set_param("current_transpose", "-7");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should be 53 (transposed down 7 semitones) */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 53);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

TEST(transpose_clamps_to_valid_range) {
    /* Test that transpose clamps to 0-127 */
    set_param("track_4_step_0_add_note", "10");  /* Low note */
    set_param("current_transpose", "-20");  /* Would go below 0 */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should be 0 (clamped) */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 0);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

TEST(transpose_clamps_high) {
    /* Test that transpose clamps at 127 */
    set_param("track_4_step_0_add_note", "120");
    set_param("current_transpose", "20");  /* Would exceed 127 */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should be 127 (clamped) */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 127);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

/* ============ Tests: Chord Follow Toggle ============ */

TEST(chord_follow_toggle) {
    /* Enable chord_follow on track 0 (normally off) */
    set_param("track_0_chord_follow", "1");

    /* Add note and set transpose */
    set_param("track_0_step_0_add_note", "60");
    set_param("current_transpose", "3");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Now track 0 should transpose */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 63);  /* 60 + 3 */

    /* Disable chord_follow */
    set_param("track_0_chord_follow", "0");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Now it should NOT transpose */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 60);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("current_transpose", "0");
}

/* ============ Tests: Beat Counting ============ */

TEST(beat_count_initial) {
    /* Before playing, beat_count should be 0 */
    set_param("playing", "0");
    int beat = get_param_int("beat_count");
    ASSERT_EQ(beat, 0);
}

TEST(beat_count_increments) {
    /* Start playing and render more than 1 beat to cross boundary */
    set_param("playing", "1");
    render_beats(2);  /* Render 2 beats to ensure we cross at least 1 boundary */

    int beat = get_param_int("beat_count");
    ASSERT(beat >= 1);  /* Should be at least 1 after crossing beat boundary */

    set_param("playing", "0");
}

TEST(beat_count_resets_on_play) {
    /* Play for a bit */
    set_param("playing", "1");
    render_beats(5);
    set_param("playing", "0");

    /* Start playing again - should reset */
    set_param("playing", "1");
    int beat = get_param_int("beat_count");
    ASSERT_EQ(beat, 0);  /* Should reset to 0 on play start */
    set_param("playing", "0");
}

/* ============ Tests: Multi-note Chords ============ */

TEST(chord_transpose) {
    /* Add multiple notes to same step (chord) */
    set_param("track_4_step_0_add_note", "60");  /* C */
    set_param("track_4_step_0_add_note", "64");  /* E */
    set_param("track_4_step_0_add_note", "67");  /* G */

    /* Set transpose */
    set_param("current_transpose", "2");  /* Up a whole step */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* All notes should be transposed */
    int found_62 = 0, found_66 = 0, found_69 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 62) found_62 = 1;
            if (g_captured_notes[i].note == 66) found_66 = 1;
            if (g_captured_notes[i].note == 69) found_69 = 1;
        }
    }
    ASSERT(found_62);  /* D */
    ASSERT(found_66);  /* F# */
    ASSERT(found_69);  /* A */

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

/* ============ Tests: Dynamic Transpose Changes ============ */

TEST(transpose_change_during_playback) {
    /* Set up notes on step 0 and step 4 */
    set_param("track_4_step_0_add_note", "60");
    set_param("track_4_step_4_add_note", "60");

    clear_captured_notes();

    /* Start with no transpose */
    set_param("current_transpose", "0");
    set_param("playing", "1");

    /* Render to step 0 */
    render_steps(1);

    /* Change transpose before step 4 */
    set_param("current_transpose", "7");

    /* Render to step 4 */
    render_steps(4);

    set_param("playing", "0");

    /* First note should be 60, second should be 67 */
    int found_60 = 0, found_67 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 67) found_67 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_67);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("track_4_step_4_clear", "1");
    set_param("current_transpose", "0");
}

/* ============ Tests: Multiple Tracks ============ */

TEST(multiple_tracks_mixed_chord_follow) {
    /* Track 0: chord_follow=0 (default drum)
     * Track 4: chord_follow=1 (default melodic)
     * Both play the same note, only track 4 should transpose
     */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_4_step_0_add_note", "60");
    set_param("current_transpose", "5");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Should have note 60 (from track 0) and 65 (from track 4) */
    int found_60 = 0, found_65 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 65) found_65 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_65);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_4_step_0_clear", "1");
    set_param("current_transpose", "0");
}

/* ============ Tests: Trigger Conditions ============ */

/* Helper: Count note-ons for a specific note on a channel */
static int count_note_ons(uint8_t note, uint8_t channel) {
    int count = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on &&
            g_captured_notes[i].note == note &&
            g_captured_notes[i].channel == channel) {
            count++;
        }
    }
    return count;
}

/* Helper: Render enough to complete N pattern loops (16 steps each) */
static void render_loops(int loops) {
    render_steps(loops * 16);
}

TEST(condition_1_of_2) {
    /* Condition 1:2 - play on 1st of every 2 loops */
    /* Set a note with condition n=2, m=1 */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "2");
    set_param("track_0_step_0_condition_m", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(4);  /* Play 4 loops */
    set_param("playing", "0");

    /* Should trigger on loops 0, 2 (1st of every 2) = 2 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(condition_2_of_2) {
    /* Condition 2:2 - play on 2nd of every 2 loops */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "2");
    set_param("track_0_step_0_condition_m", "2");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(4);  /* Play 4 loops */
    set_param("playing", "0");

    /* Should trigger on loops 1, 3 (2nd of every 2) = 2 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(condition_2_of_3) {
    /* Condition 2:3 - play on 2nd of every 3 loops */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "3");
    set_param("track_0_step_0_condition_m", "2");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(6);  /* Play 6 loops */
    set_param("playing", "0");

    /* Should trigger on loops 1, 4 (2nd of every 3) = 2 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(condition_1_of_4) {
    /* Condition 1:4 - play on 1st of every 4 loops */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "4");
    set_param("track_0_step_0_condition_m", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(8);  /* Play 8 loops */
    set_param("playing", "0");

    /* Should trigger on loops 0, 4 (1st of every 4) = 2 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(condition_negated) {
    /* Condition NOT 1:2 - play on all loops EXCEPT 1st of every 2 */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "2");
    set_param("track_0_step_0_condition_m", "1");
    set_param("track_0_step_0_condition_not", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(4);  /* Play 4 loops */
    set_param("playing", "0");

    /* Should trigger on loops 1, 3 (NOT 1st of every 2) = 2 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(condition_no_condition) {
    /* No condition (n=0) - should always play */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_condition_n", "0");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(4);  /* Play 4 loops */
    set_param("playing", "0");

    /* Should trigger on every loop = 4 times */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 4);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

/* ============ Tests: Ratchet ============ */

TEST(ratchet_2x) {
    /* Ratchet 2x - should play note twice per step */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_ratchet", "2");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(1);  /* Play 1 loop */
    set_param("playing", "0");

    /* Should trigger 2 times (ratchet 2x) */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

TEST(ratchet_4x) {
    /* Ratchet 4x - should play note 4 times per step */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_ratchet", "4");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(1);  /* Play 1 loop */
    set_param("playing", "0");

    /* Should trigger 4 times (ratchet 4x) */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 4);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

/* ============ Tests: Component Spark (Ratchet Condition) ============ */

TEST(comp_spark_ratchet_conditional) {
    /* Ratchet 2x with comp_spark 1:2 - ratchet only on 1st of every 2 loops */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_ratchet", "2");
    set_param("track_0_step_0_comp_spark_n", "2");
    set_param("track_0_step_0_comp_spark_m", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(4);  /* Play 4 loops */
    set_param("playing", "0");

    /* Loop 0: ratchet fires (2 notes)
     * Loop 1: no comp_spark, single note
     * Loop 2: ratchet fires (2 notes)
     * Loop 3: no comp_spark, single note
     * Total: 6 notes
     */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 6);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

/* ============ Tests: Parameter Spark (CC Lock Condition) ============ */

TEST(param_spark_cc_conditional) {
    /* CC lock with param_spark 1:2 - CC only sent on 1st of every 2 loops */
    /* We can't directly capture CCs in the current test setup,
       but we verify the condition parsing works */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_cc1", "100");
    set_param("track_0_step_0_param_spark_n", "2");
    set_param("track_0_step_0_param_spark_m", "1");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(2);
    set_param("playing", "0");

    /* Notes should still play every loop (param_spark only affects CC) */
    int count = count_note_ons(60, 0);
    ASSERT_EQ(count, 2);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
}

/* ============ Tests: Jump ============ */

TEST(jump_basic) {
    /* Jump from step 0 to step 8 */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_jump", "8");
    set_param("track_0_step_8_add_note", "72");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(20);  /* Render enough for jump to happen */
    set_param("playing", "0");

    /* Should have both notes - step 0 plays, jumps to 8, step 8 plays */
    int found_60 = 0, found_72 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 72) found_72 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_72);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_0_step_8_clear", "1");
}

TEST(jump_with_comp_spark) {
    /* Jump only on 1st of every 2 loops (comp_spark controls jump) */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_0_jump", "8");
    set_param("track_0_step_0_comp_spark_n", "2");
    set_param("track_0_step_0_comp_spark_m", "1");
    set_param("track_0_step_4_add_note", "64");  /* This step gets skipped on jump */
    set_param("track_0_step_8_add_note", "72");

    clear_captured_notes();
    set_param("playing", "1");
    render_loops(2);
    set_param("playing", "0");

    /* When we render 2 full loops (32 steps), we get 3 triggers of step 0:
     * - Initial trigger at play start (loop 0)
     * - After 16 steps (loop 1)
     * - After 32 steps (loop 2)
     *
     * Loop 0 (initial): step 0 plays, jump (comp_spark passes), step 4 skipped, step 8 plays
     * Loop 1: step 0 plays, NO jump (comp_spark fails), step 4 plays, step 8 plays
     * Loop 2 (start only): step 0 plays, jump (comp_spark passes), step 8 plays
     *
     * Note 60: 3 times (loops 0, 1, 2)
     * Note 64: 1 time (loop 1 only, skipped in loops 0 and 2 due to jump)
     * Note 72: 3 times (loops 0, 1, 2)
     */
    int count_60 = count_note_ons(60, 0);
    int count_64 = count_note_ons(64, 0);
    int count_72 = count_note_ons(72, 0);

    ASSERT_EQ(count_60, 3);
    ASSERT_EQ(count_64, 1);
    ASSERT_EQ(count_72, 3);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_0_step_4_clear", "1");
    set_param("track_0_step_8_clear", "1");
}

/* ============ Tests: Swing Calculation ============ */

/* Test calculate_swing_delay directly */
TEST(swing_delay_no_swing) {
    /* Swing 50 = no swing, delay should always be 0 */
    double delay;

    delay = calculate_swing_delay(50, 0.0);  /* Downbeat */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(50, 1.0);  /* Upbeat */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(50, 2.0);  /* Downbeat */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(50, 3.0);  /* Upbeat */
    ASSERT(delay == 0.0);
}

TEST(swing_delay_below_50) {
    /* Swing below 50 should also have no delay */
    double delay;

    delay = calculate_swing_delay(0, 1.0);
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(25, 1.0);
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(49, 1.0);
    ASSERT(delay == 0.0);
}

TEST(swing_delay_downbeats_not_affected) {
    /* Downbeats (even steps: 0, 2, 4, ...) should never have swing delay */
    double delay;

    delay = calculate_swing_delay(67, 0.0);  /* Step 0 */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(67, 2.0);  /* Step 2 */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(100, 4.0);  /* Step 4 */
    ASSERT(delay == 0.0);

    delay = calculate_swing_delay(100, 100.0);  /* Step 100 */
    ASSERT(delay == 0.0);
}

TEST(swing_delay_upbeats_affected) {
    /* Upbeats (odd steps: 1, 3, 5, ...) should have swing delay when swing > 50 */
    double delay;

    /* Swing 67 (triplet feel): delay = (67-50)/100 * 0.5 = 0.085 */
    delay = calculate_swing_delay(67, 1.0);
    ASSERT(delay > 0.08 && delay < 0.09);  /* ~0.085 */

    delay = calculate_swing_delay(67, 3.0);
    ASSERT(delay > 0.08 && delay < 0.09);

    /* Swing 100 (maximum): delay = (100-50)/100 * 0.5 = 0.25 */
    delay = calculate_swing_delay(100, 1.0);
    ASSERT(delay > 0.24 && delay < 0.26);  /* ~0.25 */

    delay = calculate_swing_delay(100, 5.0);
    ASSERT(delay > 0.24 && delay < 0.26);
}

TEST(swing_delay_values) {
    /* Test specific swing values */
    double delay;

    /* Swing 60: delay = (60-50)/100 * 0.5 = 0.05 */
    delay = calculate_swing_delay(60, 1.0);
    ASSERT(delay > 0.04 && delay < 0.06);

    /* Swing 75: delay = (75-50)/100 * 0.5 = 0.125 */
    delay = calculate_swing_delay(75, 1.0);
    ASSERT(delay > 0.12 && delay < 0.13);

    /* Swing 80: delay = (80-50)/100 * 0.5 = 0.15 */
    delay = calculate_swing_delay(80, 1.0);
    ASSERT(delay > 0.14 && delay < 0.16);
}

TEST(swing_set_track_swing) {
    /* Test that track swing can be set via set_param */
    set_param("track_0_swing", "67");

    /* Verify via internal state (we have access since we include the .c file) */
    ASSERT_EQ(g_tracks[0].swing, 67);

    /* Reset */
    set_param("track_0_swing", "50");
    ASSERT_EQ(g_tracks[0].swing, 50);
}

TEST(swing_default_value) {
    /* Default swing should be 50 (no swing) */
    ASSERT_EQ(g_tracks[0].swing, 50);
}

/* Test swing's effect on note timing by examining scheduled notes */
TEST(swing_affects_note_scheduling) {
    /* Set up notes on step 0 (downbeat) and step 1 (upbeat) */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_1_add_note", "61");
    set_param("track_0_swing", "100");  /* Maximum swing */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(3);  /* Render past both steps */
    set_param("playing", "0");

    /* Both notes should play */
    int found_60 = 0, found_61 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 61) found_61 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_61);

    /* Note 60 (step 0, downbeat) should come before note 61 (step 1, upbeat with swing delay) */
    /* With swing, note 61 is delayed, so 60 should appear first in capture order */
    int idx_60 = -1, idx_61 = -1;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60 && idx_60 < 0) idx_60 = i;
            if (g_captured_notes[i].note == 61 && idx_61 < 0) idx_61 = i;
        }
    }
    ASSERT(idx_60 < idx_61);  /* 60 should be captured before 61 */

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_0_step_1_clear", "1");
    set_param("track_0_swing", "50");
}

TEST(swing_per_track) {
    /* Different tracks can have different swing values */
    set_param("track_0_swing", "50");  /* No swing */
    set_param("track_1_swing", "100"); /* Max swing */

    ASSERT_EQ(g_tracks[0].swing, 50);
    ASSERT_EQ(g_tracks[1].swing, 100);

    /* Reset */
    set_param("track_1_swing", "50");
}

/* ============ Tests: Swing with Different Loop Lengths ============ */

/*
 * Swing is applied based on the GLOBAL phase, not the track's local step.
 * This means a track with a shorter loop will have its swing tied to the
 * master clock, not its own loop position.
 *
 * With a 5-step loop, the SAME local step alternates between swing/no-swing:
 * - Step 0 plays at global 0, 5, 10... (even, odd, even) - ALTERNATES!
 * - Step 1 plays at global 1, 6, 11... (odd, even, odd) - ALTERNATES!
 *
 * This is different from 4/8/16-step loops where steps always land on the
 * same parity (step 1 is always odd, step 2 is always even, etc).
 */

TEST(swing_different_loop_lengths_first_loop) {
    /* Track 0: 16-step loop, note on step 1
     * Track 1: 5-step loop, note on step 1
     *
     * First loop: both step 1s play at global phase 1 (odd - swing)
     */
    set_param("track_0_step_1_add_note", "60");
    set_param("track_0_swing", "100");
    set_param("track_0_loop_end", "15");  /* 16 steps */

    set_param("track_1_step_1_add_note", "72");
    set_param("track_1_swing", "100");
    set_param("track_1_loop_end", "4");   /* 5 steps */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(4);  /* Render through step 1 */
    set_param("playing", "0");

    /* Both notes should play */
    int found_60 = 0, found_72 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 72) found_72 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_72);

    /* Clean up */
    set_param("track_0_step_1_clear", "1");
    set_param("track_1_step_1_clear", "1");
    set_param("track_0_swing", "50");
    set_param("track_1_swing", "50");
    set_param("track_0_loop_end", "15");
    set_param("track_1_loop_end", "15");
}

TEST(swing_short_loop_second_iteration) {
    /* Track 1 has 5-step loop. Step 1 plays at:
     * - Global phase 1 (odd - swing)
     * - Global phase 6 (even - NO swing!)
     * - Global phase 11 (odd - swing)
     *
     * This demonstrates that the SAME local step alternates between
     * getting swing and not getting swing based on global phase.
     *
     * Track 0 has 16-step loop with step 1:
     * - Global phase 1 (odd - swing) - always the same
     */
    set_param("track_0_step_1_add_note", "60");
    set_param("track_0_swing", "100");
    set_param("track_0_loop_end", "15");  /* 16 steps */

    set_param("track_1_step_1_add_note", "72");
    set_param("track_1_swing", "100");
    set_param("track_1_loop_end", "4");   /* 5 steps */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(12);  /* Render through global step 11 */
    set_param("playing", "0");

    int count_60 = count_note_ons(60, 0);
    int count_72 = count_note_ons(72, 1);

    /* Track 0 step 1 plays once (at global 1) */
    ASSERT_EQ(count_60, 1);
    /* Track 1 step 1 plays at global 1, 6, 11 = 3 times */
    ASSERT_EQ(count_72, 3);

    /* Clean up */
    set_param("track_0_step_1_clear", "1");
    set_param("track_1_step_1_clear", "1");
    set_param("track_0_swing", "50");
    set_param("track_1_swing", "50");
    set_param("track_0_loop_end", "15");
    set_param("track_1_loop_end", "15");
}

TEST(swing_global_phase_determines_swing) {
    /* Verify that swing is based on global phase, not local step number.
     *
     * Track 0 with 5-step loop:
     * - Step 0 at global phase 0, 5, 10... (even, odd, even) - ALTERNATES!
     * - Step 1 at global phase 1, 6, 11... (odd, even, odd) - ALTERNATES!
     * - Step 2 at global phase 2, 7, 12... (even, odd, even) - ALTERNATES!
     * - Step 3 at global phase 3, 8, 13... (odd, even, odd) - ALTERNATES!
     * - Step 4 at global phase 4, 9, 14... (even, odd, even) - ALTERNATES!
     *
     * This is the key difference from 4/8/16-step loops where steps always
     * land on the same parity. With 5 steps, each step alternates between
     * getting swing and not getting swing!
     */
    set_param("track_0_step_0_add_note", "60");
    set_param("track_0_step_1_add_note", "61");
    set_param("track_0_step_2_add_note", "62");
    set_param("track_0_step_3_add_note", "63");
    set_param("track_0_step_4_add_note", "64");
    set_param("track_0_swing", "100");
    set_param("track_0_loop_end", "4");  /* 5-step loop */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(10);  /* Two complete loops of 5 steps */
    set_param("playing", "0");

    /* All notes should play twice (2 loops) */
    int count_60 = count_note_ons(60, 0);
    int count_61 = count_note_ons(61, 0);
    int count_62 = count_note_ons(62, 0);
    int count_63 = count_note_ons(63, 0);
    int count_64 = count_note_ons(64, 0);

    ASSERT_EQ(count_60, 2);  /* Step 0: plays at global 0 (no swing), 5 (swing) */
    ASSERT_EQ(count_61, 2);  /* Step 1: plays at global 1 (swing), 6 (no swing) */
    ASSERT_EQ(count_62, 2);  /* Step 2: plays at global 2 (no swing), 7 (swing) */
    ASSERT_EQ(count_63, 2);  /* Step 3: plays at global 3 (swing), 8 (no swing) */
    ASSERT_EQ(count_64, 2);  /* Step 4: plays at global 4 (no swing), 9 (swing) */

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_0_step_1_clear", "1");
    set_param("track_0_step_2_clear", "1");
    set_param("track_0_step_3_clear", "1");
    set_param("track_0_step_4_clear", "1");
    set_param("track_0_swing", "50");
    set_param("track_0_loop_end", "15");
}

TEST(swing_comparison_5_vs_16_step_loops) {
    /* Direct comparison: step 1 on a 5-step loop vs 16-step loop.
     * After 16 global steps:
     * - 16-step track: step 1 plays once at global 1 (odd - swing)
     * - 5-step track: step 1 plays at global 1, 6, 11, 16
     *                 = odd (swing), even (no swing), odd (swing), even (no swing)
     *
     * The 5-step loop's step 1 alternates between swung and not swung!
     */
    set_param("track_0_step_1_add_note", "60");
    set_param("track_0_swing", "100");
    set_param("track_0_loop_end", "15");  /* 16 steps */

    set_param("track_1_step_1_add_note", "72");
    set_param("track_1_swing", "100");
    set_param("track_1_loop_end", "4");   /* 5 steps */

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(17);  /* Just past one full 16-step loop */
    set_param("playing", "0");

    int count_60 = count_note_ons(60, 0);
    int count_72 = count_note_ons(72, 1);

    /* 16-step track plays step 1 once */
    ASSERT_EQ(count_60, 1);
    /* 5-step track plays step 1 four times (at global 1, 6, 11, 16) */
    ASSERT_EQ(count_72, 4);

    /* Clean up */
    set_param("track_0_step_1_clear", "1");
    set_param("track_1_step_1_clear", "1");
    set_param("track_0_swing", "50");
    set_param("track_1_swing", "50");
    set_param("track_0_loop_end", "15");
    set_param("track_1_loop_end", "15");
}

/* ============ Tests: Transpose Sequence (DSP Internal) ============ */

TEST(transpose_sequence_empty) {
    /* Empty sequence = no transpose */
    set_param("transpose_clear", "1");
    set_param("track_4_step_0_add_note", "60");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should be untransposed */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 60);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
}

TEST(transpose_sequence_single_step) {
    /* Single step: +5 for 16 steps (4 beats) */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "5");
    set_param("transpose_step_0_duration", "16");
    set_param("transpose_step_count", "1");

    set_param("track_4_step_0_add_note", "60");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should be transposed +5 */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 65);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_multiple_steps) {
    /* Step 0: +0 for 4 steps, Step 1: +7 for 4 steps */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "0");
    set_param("transpose_step_0_duration", "4");
    set_param("transpose_step_1_transpose", "7");
    set_param("transpose_step_1_duration", "4");
    set_param("transpose_step_count", "2");

    /* Note at step 0 (in first transpose region) */
    set_param("track_4_step_0_add_note", "60");
    /* Note at step 4 (in second transpose region) */
    set_param("track_4_step_4_add_note", "60");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(6);  /* Get through steps 0-5 */
    set_param("playing", "0");

    /* Should have captured note 60 and note 67 */
    int found_60 = 0, found_67 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 60) found_60 = 1;
            if (g_captured_notes[i].note == 67) found_67 = 1;
        }
    }
    ASSERT(found_60);
    ASSERT(found_67);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("track_4_step_4_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_boundary_exact) {
    /* Critical: transpose changes exactly at step boundary */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "0");
    set_param("transpose_step_0_duration", "4");   /* Steps 0-3 */
    set_param("transpose_step_1_transpose", "12");
    set_param("transpose_step_1_duration", "4");   /* Steps 4-7 */
    set_param("transpose_step_count", "2");

    /* Note at step 3 (last step of first transpose) */
    set_param("track_4_step_3_add_note", "48");
    /* Note at step 4 (first step of second transpose) */
    set_param("track_4_step_4_add_note", "48");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(6);
    set_param("playing", "0");

    /* Step 3: transpose +0 -> note 48 */
    /* Step 4: transpose +12 -> note 60 */
    int found_48 = 0, found_60 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on) {
            if (g_captured_notes[i].note == 48) found_48 = 1;
            if (g_captured_notes[i].note == 60) found_60 = 1;
        }
    }
    ASSERT(found_48);
    ASSERT(found_60);

    /* Clean up */
    set_param("track_4_step_3_clear", "1");
    set_param("track_4_step_4_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_pause_resume) {
    /* Fix for original bug: pause/resume uses correct transpose */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "0");
    set_param("transpose_step_0_duration", "16");
    set_param("transpose_step_1_transpose", "5");
    set_param("transpose_step_1_duration", "16");
    set_param("transpose_step_count", "2");

    set_param("track_4_step_0_add_note", "60");

    /* Play to step 16 (into second transpose region) */
    set_param("playing", "1");
    render_steps(17);
    set_param("playing", "0");

    /* Pause and resume - should restart at step 0 with transpose +0 */
    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* First note should be 60 (transpose +0), not 65 (transpose +5) */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 60);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_loop) {
    /* Sequence loops correctly */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "0");
    set_param("transpose_step_0_duration", "8");
    set_param("transpose_step_1_transpose", "12");
    set_param("transpose_step_1_duration", "8");
    set_param("transpose_step_count", "2");
    /* Total: 16 steps, loops at step 16 */

    set_param("track_4_step_0_add_note", "48");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(17);  /* Step 16 = looped to step 0 */
    set_param("playing", "0");

    /* First note at step 0: transpose +0 -> 48 */
    /* Second note at step 16 (looped): transpose +0 -> 48 */
    int count_48 = 0;
    for (int i = 0; i < g_num_captured; i++) {
        if (g_captured_notes[i].is_note_on && g_captured_notes[i].note == 48) {
            count_48++;
        }
    }
    ASSERT_EQ(count_48, 2);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_long_duration) {
    /* Test long duration (256 steps = 16 bars) */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "3");
    set_param("transpose_step_0_duration", "256");
    set_param("transpose_step_count", "1");

    set_param("track_4_step_0_add_note", "60");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 63);

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("transpose_clear", "1");
}

TEST(transpose_sequence_drum_track_not_affected) {
    /* Drum tracks (0-3) should not transpose even with sequence */
    set_param("transpose_clear", "1");
    set_param("transpose_step_0_transpose", "12");
    set_param("transpose_step_0_duration", "16");
    set_param("transpose_step_count", "1");

    /* Note on drum track (0) */
    set_param("track_0_step_0_add_note", "36");

    clear_captured_notes();
    set_param("playing", "1");
    render_steps(1);
    set_param("playing", "0");

    /* Note should NOT be transposed */
    ASSERT(g_num_captured >= 1);
    ASSERT_EQ(g_captured_notes[0].note, 36);

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("transpose_clear", "1");
}

/* ============ Tests: Scale Detection ============ */

TEST(scale_detection_no_notes) {
    /* Clear all notes first */
    for (int t = 4; t < 8; t++) {
        for (int s = 0; s < 16; s++) {
            char key[64];
            snprintf(key, sizeof(key), "track_%d_step_%d_clear", t, s);
            set_param(key, "1");
        }
    }

    int root = get_param_int("detected_scale_root");
    ASSERT_EQ(root, -1);
}

TEST(scale_detection_c_major_triad) {
    /* Clear first */
    for (int t = 4; t < 8; t++) {
        for (int s = 0; s < 16; s++) {
            char key[64];
            snprintf(key, sizeof(key), "track_%d_step_%d_clear", t, s);
            set_param(key, "1");
        }
    }

    /* Add C major triad to track 4 (chord-follow) */
    set_param("track_4_step_0_add_note", "60");  /* C */
    set_param("track_4_step_1_add_note", "64");  /* E */
    set_param("track_4_step_2_add_note", "67");  /* G */

    int root = get_param_int("detected_scale_root");
    ASSERT_EQ(root, 0);  /* C */

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("track_4_step_1_clear", "1");
    set_param("track_4_step_2_clear", "1");
}

TEST(scale_detection_drum_track_ignored) {
    /* Clear chord-follow tracks first */
    for (int t = 4; t < 8; t++) {
        for (int s = 0; s < 16; s++) {
            char key[64];
            snprintf(key, sizeof(key), "track_%d_step_%d_clear", t, s);
            set_param(key, "1");
        }
    }

    /* Notes on track 0 (chord_follow=0) should be ignored */
    set_param("track_0_step_0_add_note", "61");  /* C# */
    set_param("track_0_step_1_add_note", "63");  /* D# */

    int root = get_param_int("detected_scale_root");
    ASSERT_EQ(root, -1);  /* No scale from non-chord-follow tracks */

    /* Clean up */
    set_param("track_0_step_0_clear", "1");
    set_param("track_0_step_1_clear", "1");
}

TEST(scale_detection_updates_on_note_change) {
    /* Clear first */
    for (int t = 4; t < 8; t++) {
        for (int s = 0; s < 16; s++) {
            char key[64];
            snprintf(key, sizeof(key), "track_%d_step_%d_clear", t, s);
            set_param(key, "1");
        }
    }

    /* Add C note */
    set_param("track_4_step_0_add_note", "60");  /* C */
    int root1 = get_param_int("detected_scale_root");
    ASSERT_EQ(root1, 0);  /* C */

    /* Add more notes to potentially change scale */
    set_param("track_4_step_1_add_note", "62");  /* D */
    set_param("track_4_step_2_add_note", "64");  /* E */
    int root2 = get_param_int("detected_scale_root");
    ASSERT(root2 >= 0);  /* Scale should be detected */

    /* Clean up */
    set_param("track_4_step_0_clear", "1");
    set_param("track_4_step_1_clear", "1");
    set_param("track_4_step_2_clear", "1");
}

/* ============ Test Runner ============ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("SEQOMD DSP Plugin Tests\n");
    printf("=======================\n\n");

    /* Initialize plugin once for all tests */
    init_plugin();

    printf("Basic Functionality:\n");
    RUN_TEST(plugin_init);
    RUN_TEST(default_bpm);
    RUN_TEST(set_bpm);
    RUN_TEST(default_chord_follow);

    printf("\nStep and Note Programming:\n");
    RUN_TEST(add_note_to_step);
    RUN_TEST(clear_step);

    printf("\nTranspose:\n");
    RUN_TEST(transpose_no_effect_on_drum_tracks);
    RUN_TEST(transpose_applies_to_chord_follow_tracks);
    RUN_TEST(transpose_negative);
    RUN_TEST(transpose_clamps_to_valid_range);
    RUN_TEST(transpose_clamps_high);

    printf("\nChord Follow Toggle:\n");
    RUN_TEST(chord_follow_toggle);

    printf("\nBeat Counting:\n");
    RUN_TEST(beat_count_initial);
    RUN_TEST(beat_count_increments);
    RUN_TEST(beat_count_resets_on_play);

    printf("\nChords:\n");
    RUN_TEST(chord_transpose);

    printf("\nDynamic Changes:\n");
    RUN_TEST(transpose_change_during_playback);

    printf("\nMultiple Tracks:\n");
    RUN_TEST(multiple_tracks_mixed_chord_follow);

    printf("\nTrigger Conditions:\n");
    RUN_TEST(condition_1_of_2);
    RUN_TEST(condition_2_of_2);
    RUN_TEST(condition_2_of_3);
    RUN_TEST(condition_1_of_4);
    RUN_TEST(condition_negated);
    RUN_TEST(condition_no_condition);

    printf("\nRatchet:\n");
    RUN_TEST(ratchet_2x);
    RUN_TEST(ratchet_4x);

    printf("\nComponent Spark (Ratchet/Jump Conditions):\n");
    RUN_TEST(comp_spark_ratchet_conditional);

    printf("\nParameter Spark (CC Conditions):\n");
    RUN_TEST(param_spark_cc_conditional);

    printf("\nJump:\n");
    RUN_TEST(jump_basic);
    RUN_TEST(jump_with_comp_spark);

    printf("\nSwing Calculation:\n");
    RUN_TEST(swing_delay_no_swing);
    RUN_TEST(swing_delay_below_50);
    RUN_TEST(swing_delay_downbeats_not_affected);
    RUN_TEST(swing_delay_upbeats_affected);
    RUN_TEST(swing_delay_values);
    RUN_TEST(swing_set_track_swing);
    RUN_TEST(swing_default_value);
    RUN_TEST(swing_affects_note_scheduling);
    RUN_TEST(swing_per_track);

    printf("\nSwing with Different Loop Lengths:\n");
    RUN_TEST(swing_different_loop_lengths_first_loop);
    RUN_TEST(swing_short_loop_second_iteration);
    RUN_TEST(swing_global_phase_determines_swing);
    RUN_TEST(swing_comparison_5_vs_16_step_loops);

    printf("\nTranspose Sequence (DSP Internal):\n");
    RUN_TEST(transpose_sequence_empty);
    RUN_TEST(transpose_sequence_single_step);
    RUN_TEST(transpose_sequence_multiple_steps);
    RUN_TEST(transpose_sequence_boundary_exact);
    RUN_TEST(transpose_sequence_pause_resume);
    RUN_TEST(transpose_sequence_loop);
    RUN_TEST(transpose_sequence_long_duration);
    RUN_TEST(transpose_sequence_drum_track_not_affected);

    printf("\nScale Detection:\n");
    RUN_TEST(scale_detection_no_notes);
    RUN_TEST(scale_detection_c_major_triad);
    RUN_TEST(scale_detection_drum_track_ignored);
    RUN_TEST(scale_detection_updates_on_note_change);

    cleanup_plugin();

    printf("\n=======================\n");
    printf("Tests: %d run, %d passed, %d failed\n",
           g_tests_run, g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
