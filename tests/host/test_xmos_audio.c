/* Unit test for the XMOS audio-IO SysEx codec behind USB-C out persistence.
 *
 * Wire bytes come from a live capture on hardware, 2026-08-18 (see
 * docs/superpowers/specs/2026-08-18-usbc-out-source-capture.txt):
 *   Main Out: F0 00 21 1D 01 01 37 12 02 00x12 F7  +  ... 37 14 01 ...
 *   Mic:      F0 00 21 1D 01 01 37 12 00 00x12 F7  +  ... 37 14 00 ...
 *
 * Build/run: bash tests/host/test_xmos_audio.sh
 */
#include <stdio.h>
#include <string.h>
#include "shadow_xmos_audio.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

#define MIDI_OUT_LEN 80  /* 20 slots x 4 bytes */

/* Append one 23-byte audio-IO message to buf at *slot, mirroring Move's own
 * framing: 7 packets of cin 0x4, then a cin 0x6 end packet. */
static void put_msg(uint8_t *buf, int *slot, uint8_t key, uint8_t val) {
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };
    memcpy(msg, hdr, sizeof hdr);
    msg[7] = key;
    msg[8] = val;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;

    int pos = 0;
    while (pos < XMOS_AUDIO_MSG_LEN) {
        int remaining = XMOS_AUDIO_MSG_LEN - pos;
        int n = remaining >= 3 ? 3 : remaining;
        buf[*slot] = (remaining > 3) ? 0x04 : (n == 3 ? 0x07 : (n == 2 ? 0x06 : 0x05));
        buf[*slot + 1] = msg[pos];
        buf[*slot + 2] = n > 1 ? msg[pos + 1] : 0;
        buf[*slot + 3] = n > 2 ? msg[pos + 2] : 0;
        pos += n;
        *slot += 4;
    }
}

/* A full frame as Move sends it: the 37 12 message then the 37 14 message. */
static void make_frame(uint8_t *buf, uint8_t route_val, uint8_t mon_val) {
    memset(buf, 0, MIDI_OUT_LEN);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, route_val);
    put_msg(buf, &slot, XMOS_AUDIO_KEY_MON, mon_val);
}

static xmos_audio_state_t fresh(void) {
    xmos_audio_state_t st;
    memset(&st, 0, sizeof st);
    st.usbc_out = -1;
    return st;
}

static void test_main_out(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = fresh();
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 1, "main out: scan reports change");
    CHECK(st.usbc_out == 1, "main out: usbc_out == 1");
    CHECK(st.seq == 1, "main out: seq bumped once");
    CHECK(st.have_route == 1, "main out: route payload captured");
    CHECK(st.route[8] == 0x02, "main out: route value byte preserved");
}

static void test_mic(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x00, 0x00);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 0, "mic: usbc_out == 0");
}

static void test_idempotent(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "idempotent: second scan reports no change");
    CHECK(st.seq == 1, "idempotent: seq not bumped twice");
}

static void test_ignores_led_sysex(void) {
    /* LED RGB SysEx is the same manufacturer envelope with cmd 0x3B. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    buf[4] = 0x04; buf[5] = 0x1D; buf[6] = 0x01; buf[7] = 0x01;
    buf[8] = 0x04; buf[9] = 0x3B; buf[10] = 0x00; buf[11] = 0x05;
    buf[12] = 0x06; buf[13] = 0x00; buf[14] = 0xF7; buf[15] = 0x00;
    xmos_audio_state_t st = fresh();
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "led sysex: no change reported");
    CHECK(st.usbc_out == -1, "led sysex: usbc_out untouched");
}

static void test_rejects_truncated(void) {
    /* Envelope that never reaches F7 — must not be accepted. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    buf[4] = 0x04; buf[5] = 0x1D; buf[6] = 0x01; buf[7] = 0x01;
    buf[8] = 0x04; buf[9] = 0x37; buf[10] = 0x14; buf[11] = 0x01;
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == -1, "truncated: usbc_out untouched");
}

static void test_interleaved_non_sysex(void) {
    /* A CC packet lands between two fragments; reassembly must survive it. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_MON, 0x01);
    /* Shift the last packet one slot later, dropping a CC into the gap. */
    memcpy(&buf[slot], &buf[slot - 4], 4);
    buf[slot - 4] = 0x0B; buf[slot - 3] = 0xB0; buf[slot - 2] = 0x4F; buf[slot - 1] = 0x40;
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "interleaved: still parsed");
}

static void test_build_preserves_input_route_bit(void) {
    uint8_t buf[MIDI_OUT_LEN];
    /* Move last selected USB-C *input* (bit0) with monitoring off. */
    make_frame(buf, 0x01, 0x00);
    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);

    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[8] == 0x03, "build: monitor bit set, input route bit kept");
    CHECK(mon[7] == XMOS_AUDIO_KEY_MON && mon[8] == 0x01, "build: mon message correct");

    xmos_audio_build(&st, 0, route, mon);
    CHECK(route[8] == 0x01, "build: monitor bit cleared, input route bit kept");
    CHECK(mon[8] == 0x00, "build: mon message cleared");
}

static void test_build_without_observation(void) {
    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[0] == 0xF0 && route[6] == 0x37, "build cold: header present");
    CHECK(route[7] == XMOS_AUDIO_KEY_ROUTE, "build cold: route key");
    CHECK(route[8] == 0x02, "build cold: only monitor bit set");
    CHECK(route[XMOS_AUDIO_MSG_LEN - 1] == 0xF7, "build cold: terminated");
}

static void test_emit_round_trip(void) {
    xmos_audio_state_t src = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&src, 1, route, mon);

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit: route accepted");
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, mon) == 1, "emit: mon accepted");

    xmos_audio_state_t st = fresh();
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "round trip: emitted pair scans back as main out");
}

static void test_emit_respects_occupied_slots(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* Occupy the first three slots with LED-ish traffic. */
    for (int i = 0; i < 12; i += 4) {
        buf[i] = 0x0B; buf[i+1] = 0xB0; buf[i+2] = 0x10; buf[i+3] = 0x7F;
    }
    uint8_t snapshot[12];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit: fits around occupied slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: occupied slots untouched");
}

static void test_emit_defers_when_full(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* Leave only 7 free slots — one short of a message. */
    for (int i = 0; i < MIDI_OUT_LEN - 28; i += 4) {
        buf[i] = 0x0B; buf[i+1] = 0xB0; buf[i+2] = 0x10; buf[i+3] = 0x7F;
    }
    uint8_t snapshot[MIDI_OUT_LEN];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = fresh();
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 0, "emit: refuses when short on slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: buffer untouched on refusal");
}

int main(void) {
    test_main_out();
    test_mic();
    test_idempotent();
    test_ignores_led_sysex();
    test_rejects_truncated();
    test_interleaved_non_sysex();
    test_build_preserves_input_route_bit();
    test_build_without_observation();
    test_emit_round_trip();
    test_emit_respects_occupied_slots();
    test_emit_defers_when_full();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
