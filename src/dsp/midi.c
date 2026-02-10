/*
 * SEQOMD DSP Plugin - MIDI Functions
 *
 * MIDI message sending utilities.
 */

#include "seq_plugin.h"

void send_note_on(int note, int velocity, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x29,                           /* Cable 2, CIN 0x9 (Note On) */
        (uint8_t)(0x90 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        (uint8_t)(velocity & 0x7F)
    };
    g_host->midi_send_external(msg, 4);
}

void send_note_off(int note, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x28,                           /* Cable 2, CIN 0x8 (Note Off) */
        (uint8_t)(0x80 | (channel & 0x0F)),
        (uint8_t)(note & 0x7F),
        0x00
    };
    g_host->midi_send_external(msg, 4);
}

void send_cc(int cc, int value, int channel) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = {
        0x2B,                           /* Cable 2, CIN 0xB (Control Change) */
        (uint8_t)(0xB0 | (channel & 0x0F)),
        (uint8_t)(cc & 0x7F),
        (uint8_t)(value & 0x7F)
    };
    g_host->midi_send_external(msg, 4);
}

void send_midi_clock(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_CLOCK, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
}

void send_midi_start(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_START, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Start");
}

void send_midi_stop(void) {
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[4] = { 0x2F, MIDI_STOP, 0x00, 0x00 };
    g_host->midi_send_external(msg, 4);
    plugin_log("MIDI Stop");
}
