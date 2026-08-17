/* Regression test for suspend-keeps-JS overtake LED handoff.
 *
 * A native note-layout snapshot can be incomplete when an overtake starts.
 * When the tool requests a native repaint on exit, the host must not replay
 * that snapshot or any queued tool LED writes over Move's fresh output.
 */
#include <stdio.h>
#include <string.h>

#include "shadow_led_queue.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static shadow_control_t control;
static shadow_control_t *control_ptr = &control;
static uint8_t midi_out[HW_MIDI_OUT_SIZE];
static uint8_t *ui_midi;

static void begin_host(void) {
    memset(&control, 0, sizeof control);
    memset(midi_out, 0, sizeof midi_out);
    led_queue_host_t host = {
        .midi_out_buf = midi_out,
        .shadow_control = &control_ptr,
        .shadow_ui_midi_shm = &ui_midi,
        .passthrough_ccs = NULL,
    };
    led_queue_init(&host);
}

static void empty_frame(void) {
    memset(midi_out, 0, sizeof midi_out);
}

static int packet_present(uint8_t type, uint8_t data1, uint8_t data2) {
    for (int i = 0; i < HW_MIDI_OUT_SIZE; i += 4) {
        if ((midi_out[i + 1] & 0xF0) == type &&
            midi_out[i + 2] == data1 && midi_out[i + 3] == data2) return 1;
    }
    return 0;
}

static void capture_native_then_enter_overtake(void) {
    /* Cache a representative C-major pad color and Shift-row icon. */
    midi_out[0] = 0x09; midi_out[1] = 0x90; midi_out[2] = 68; midi_out[3] = 17;
    midi_out[4] = 0x0B; midi_out[5] = 0xB0; midi_out[6] = 16; midi_out[7] = 5;
    shadow_clear_move_leds_if_overtake();

    control.overtake_mode = 2;
    shadow_clear_move_leds_if_overtake();

    /* Drain the entry clear ceremony so it cannot affect exit assertions. */
    for (int i = 0; i < 12; i++) {
        empty_frame();
        shadow_flush_pending_leds();
    }
}

static void test_default_exit_still_restores_snapshot(void) {
    begin_host();
    capture_native_then_enter_overtake();

    control.overtake_mode = 0;
    empty_frame();
    shadow_clear_move_leds_if_overtake();

    int restored_pad = 0;
    int restored_shift_row = 0;
    for (int i = 0; i < 12; i++) {
        empty_frame();
        shadow_flush_pending_leds();
        restored_pad |= packet_present(0x90, 68, 17);
        restored_shift_row |= packet_present(0xB0, 16, 5);
    }
    CHECK(restored_pad, "ordinary overtake exit restores cached native pad color");
    CHECK(restored_shift_row, "ordinary overtake exit restores cached Shift-row icon");
}

static void test_native_repaint_discards_snapshot_and_tool_queue(void) {
    begin_host();
    capture_native_then_enter_overtake();

    /* Simulate one last Mono LED write waiting in the host queue. */
    shadow_queue_led(0x09, 0x90, 68, 99);

    /* JS sets this before mode=0 and leaves it for the audio thread. */
    control.skip_led_clear = 1;
    control.overtake_mode = 0;
    empty_frame();
    shadow_clear_move_leds_if_overtake();

    CHECK(control.skip_led_clear == 0,
          "audio-side exit transition consumes native repaint request");

    for (int i = 0; i < 12; i++) {
        empty_frame();
        shadow_flush_pending_leds();
        CHECK(!packet_present(0x90, 68, 17),
              "native repaint exit does not replay entry snapshot");
        CHECK(!packet_present(0x90, 68, 99),
              "native repaint exit drops queued tool LED writes");
        CHECK(!packet_present(0xB0, 16, 5),
              "native repaint exit does not replay stale Shift-row icon");
    }
}

int main(void) {
    test_default_exit_still_restores_snapshot();
    test_native_repaint_discards_snapshot_and_tool_queue();
    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    puts("PASS: overtake native LED repaint handoff");
    return 0;
}
