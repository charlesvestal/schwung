/* touch_observe_is_edge: what reaches a touch_observe generator. */
#include <stdio.h>
#include "touch_observe.h"

static int fails = 0;
static void expect(int got, int want, const char *what)
{
    if (got != want) { printf("FAIL: %s (got %d, want %d)\n", what, got, want); fails++; }
}

int main(void)
{
    /* 8-byte MIDI_IN slots: head, status, d1, d2, 4-byte timestamp. */
    for (int n = 0; n <= 7; n++) {
        uint8_t on[8]  = { 0x09, 0x90, (uint8_t)n, 127, 1, 0, 0, 0 };
        uint8_t off[8] = { 0x08, 0x80, (uint8_t)n, 0,   1, 0, 0, 0 };
        expect(touch_observe_is_edge(on), 1, "knob touch on");
        expect(touch_observe_is_edge(off), 1, "knob touch off");
    }
    uint8_t jog[8]     = { 0x09, 0x90, 9, 127, 1, 0, 0, 0 };
    uint8_t jog0[8]    = { 0x09, 0x90, 9, 0,   1, 0, 0, 0 };  /* release as vel 0 */
    expect(touch_observe_is_edge(jog), 1, "jog touch on");
    expect(touch_observe_is_edge(jog0), 1, "jog touch release as note-on vel 0");

    uint8_t vol[8]     = { 0x09, 0x90, 8, 127, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(vol), 0, "master volume touch (8) is the host's");
    uint8_t pad[8]     = { 0x09, 0x90, 10, 127, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(pad), 0, "note 10 is not a touch");
    uint8_t ext9[8]    = { 0x29, 0x90, 9, 100, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(ext9), 0, "cable-2 note 9 is an external pitch, not the jog");
    uint8_t sys9[8]    = { 0xE9, 0x90, 9, 100, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(sys9), 0, "cable-14 note 9 is not the jog");
    uint8_t cc[8]      = { 0x0B, 0xB0, 1, 64, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(cc), 0, "a CC is not a touch");
    uint8_t sx[8]      = { 0x04, 0x90, 3, 127, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(sx), 0, "a SysEx CIN is not a touch");
    uint8_t pb[8]      = { 0x0E, 0xE0, 3, 64, 1, 0, 0, 0 };
    expect(touch_observe_is_edge(pb), 0, "pitch bend is not a touch");
    uint8_t empty[8]   = { 0 };
    expect(touch_observe_is_edge(empty), 0, "empty slot");
    expect(touch_observe_is_edge(NULL), 0, "NULL");

    if (fails) return 1;
    printf("PASS: touch_observe_is_edge\n");
    return 0;
}
