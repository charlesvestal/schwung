/* Unit tests for the two predicates that gate MIDI into audio FX.
 *
 * Both run on the SCHED_FIFO 90 SPI callback, so they are pure inline code
 * with no allocation, I/O or locks — and therefore testable natively here
 * rather than only on the device.
 *
 * Build/run: bash tests/host/test_fx_midi_filter.sh
 */
#include <stdio.h>
#include "fx_midi_filter.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

/* Move's cable-0 surface note map, from CLAUDE.md "Move Hardware MIDI":
 * knob touch 0-9, steps 16-31, tracks 40-43, pads 68-99. */
static void test_pad_range(void)
{
    CHECK(move_surface_note_is_pad(68), "pad low bound 68 accepted");
    CHECK(move_surface_note_is_pad(99), "pad high bound 99 accepted");
    CHECK(move_surface_note_is_pad(80), "mid pad accepted");

    CHECK(!move_surface_note_is_pad(67), "67 just below pads rejected");
    CHECK(!move_surface_note_is_pad(100), "100 just above pads rejected");

    /* The reported bug: a step button reached every loaded audio FX because
     * the only guard was `d1 >= 10`, which exists to drop knob touch. */
    for (int n = 16; n <= 31; n++)
        CHECK(!move_surface_note_is_pad((uint8_t)n), "step button rejected");
    for (int n = 40; n <= 43; n++)
        CHECK(!move_surface_note_is_pad((uint8_t)n), "track button rejected");
    for (int n = 0; n <= 9; n++)
        CHECK(!move_surface_note_is_pad((uint8_t)n), "knob touch rejected");
}

/* Default must be All: a stored setting nobody has touched cannot change
 * what an existing ducker hears. */
static void test_channel_all_is_transparent(void)
{
    for (int ch = 0; ch < 16; ch++) {
        CHECK(fx_midi_channel_accepts(FX_MIDI_CHANNEL_ALL, (uint8_t)(0x90 | ch)),
              "All accepts every note-on channel");
        CHECK(fx_midi_channel_accepts(FX_MIDI_CHANNEL_ALL, (uint8_t)(0xB0 | ch)),
              "All accepts every CC channel");
    }
}

static void test_channel_selective(void)
{
    /* Setting is 0-based on the wire; the UI shows 1-16. */
    CHECK(fx_midi_channel_accepts(9, 0x99), "ch10 setting accepts ch10 note-on");
    CHECK(!fx_midi_channel_accepts(9, 0x90), "ch10 setting rejects ch1 note-on");
    CHECK(!fx_midi_channel_accepts(9, 0x98), "ch10 setting rejects ch9 note-on");
    CHECK(fx_midi_channel_accepts(0, 0x80), "ch1 setting accepts ch1 note-off");
    CHECK(fx_midi_channel_accepts(15, 0xEF), "ch16 setting accepts ch16 bend");
}

/* The MIDI_OUT-echo site forwards every voice message, not just notes — a
 * channel filter that exempted CC/bend would be a half-filter. */
static void test_channel_covers_all_voice_types(void)
{
    const uint8_t types[] = { 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0 };
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        CHECK(fx_midi_channel_accepts(3, (uint8_t)(types[i] | 3)),
              "matching channel accepted for every voice type");
        CHECK(!fx_midi_channel_accepts(3, (uint8_t)(types[i] | 4)),
              "non-matching channel rejected for every voice type");
    }
}

/* System messages (0xF0-0xFF) carry no channel — the low nibble is a message
 * id, so masking it would drop clock on 15 of 16 settings. Clock drives arps
 * and synced LFOs, which is not this setting's business. */
static void test_system_messages_unfiltered(void)
{
    CHECK(fx_midi_channel_accepts(9, 0xF8), "clock passes a narrowed setting");
    CHECK(fx_midi_channel_accepts(9, 0xFA), "start passes a narrowed setting");
    CHECK(fx_midi_channel_accepts(9, 0xFC), "stop passes a narrowed setting");
    CHECK(fx_midi_channel_accepts(9, 0xF0), "sysex passes a narrowed setting");
}

/* An out-of-range stored value must fail OPEN, not silently mute every FX. */
static void test_out_of_range_setting_falls_back_to_all(void)
{
    CHECK(fx_midi_channel_accepts(16, 0x90), "16 (past ch16) falls back to All");
    CHECK(fx_midi_channel_accepts(99, 0x95), "garbage falls back to All");
    CHECK(fx_midi_channel_accepts(-7, 0x95), "negative other than -1 is All");
}

int main(void)
{
    test_pad_range();
    test_channel_all_is_transparent();
    test_channel_selective();
    test_channel_covers_all_voice_types();
    test_system_messages_unfiltered();
    test_out_of_range_setting_falls_back_to_all();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("test_fx_midi_filter: all checks passed\n");
    return 0;
}
