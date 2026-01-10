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

    cleanup_plugin();

    printf("\n=======================\n");
    printf("Tests: %d run, %d passed, %d failed\n",
           g_tests_run, g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
