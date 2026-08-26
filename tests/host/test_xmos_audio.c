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

/* Append one already-built 23-byte message to buf at *slot on the given
 * cable, mirroring the real USB-MIDI framing: 7 packets of cin 0x4, then a
 * cin 0x6 end packet (2 leftover bytes). */
static void put_raw_msg(uint8_t *buf, int *slot, uint8_t cable,
                         const uint8_t msg[XMOS_AUDIO_MSG_LEN]) {
    int pos = 0;
    while (pos < XMOS_AUDIO_MSG_LEN) {
        int remaining = XMOS_AUDIO_MSG_LEN - pos;
        int n = remaining >= 3 ? 3 : remaining;
        uint8_t cin = (remaining > 3) ? 0x04 : (n == 3 ? 0x07 : (n == 2 ? 0x06 : 0x05));
        buf[*slot]     = (uint8_t)((cable << 4) | cin);
        buf[*slot + 1] = msg[pos];
        buf[*slot + 2] = n > 1 ? msg[pos + 1] : 0;
        buf[*slot + 3] = n > 2 ? msg[pos + 2] : 0;
        pos += n;
        *slot += 4;
    }
}

/* Build a 23-byte audio-IO envelope (cable 0, key/val) and fragment it into
 * buf at *slot. */
static void put_msg(uint8_t *buf, int *slot, uint8_t key, uint8_t val) {
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };
    memcpy(msg, hdr, sizeof hdr);
    msg[7] = key;
    msg[8] = val;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
    put_raw_msg(buf, slot, 0, msg);
}

/* A full frame as Move sends it: the 37 12 message then the 37 14 message. */
static void make_frame(uint8_t *buf, uint8_t route_val, uint8_t out_src_val) {
    memset(buf, 0, MIDI_OUT_LEN);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, route_val);
    put_msg(buf, &slot, XMOS_AUDIO_KEY_OUT_SRC, out_src_val);
}

/* Raw packet bytes (XMOS_AUDIO_PACKETS * 4) for a single cable-0 envelope,
 * used by the frame-boundary-split and cable-filtering tests below, which
 * need to slice the packet stream themselves. */
static void build_packets(uint8_t out[XMOS_AUDIO_PACKETS * 4], uint8_t key, uint8_t val) {
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };
    memcpy(msg, hdr, sizeof hdr);
    msg[7] = key;
    msg[8] = val;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
    int slot = 0;
    put_raw_msg(out, &slot, 0, msg);
}

static void test_main_out(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
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
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 0, "mic: usbc_out == 0");
}

static void test_idempotent(void) {
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x02, 0x01);
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "idempotent: second scan reports no change");
    CHECK(st.seq == 1, "idempotent: seq not bumped twice");
}

static void test_transition_main_out_to_mic(void) {
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    uint8_t buf[MIDI_OUT_LEN];

    make_frame(buf, 0x02, 0x01);
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1 && st.seq == 1, "transition: main out observed first");

    make_frame(buf, 0x00, 0x00);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 1, "transition: 1->0 reported as change");
    CHECK(st.usbc_out == 0, "transition: usbc_out == 0 after mic");
    CHECK(st.seq == 2, "transition: seq reaches 2");
}

static void test_ignores_led_sysex(void) {
    /* LED RGB SysEx is the same manufacturer envelope but with command byte
     * 0x3B instead of 0x37, at the same length (23 bytes) as a real audio-IO
     * message. This is the case that actually exercises command
     * discrimination — a too-short fixture would be rejected by the length
     * check alone, never reaching the command byte. */
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    msg[0] = 0xF0; msg[1] = 0x00; msg[2] = 0x21; msg[3] = 0x1D;
    msg[4] = 0x01; msg[5] = 0x01; msg[6] = 0x3B;
    msg[7] = 0x00; msg[8] = 0x05; msg[9] = 0x06; msg[10] = 0x00;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_raw_msg(buf, &slot, 0, msg);

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "led sysex: no change reported");
    CHECK(st.usbc_out == -1, "led sysex: usbc_out untouched");
    CHECK(st.have_route == 0, "led sysex: route untouched");
}

static void test_rejects_truncated(void) {
    /* Envelope that never reaches F7 — must not be accepted. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    buf[4] = 0x04; buf[5] = 0x1D; buf[6] = 0x01; buf[7] = 0x01;
    buf[8] = 0x04; buf[9] = 0x37; buf[10] = 0x14; buf[11] = 0x01;
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == -1, "truncated: usbc_out untouched");
}

static void test_interleaved_non_sysex(void) {
    /* A CC packet lands between two fragments; reassembly must survive it. */
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_OUT_SRC, 0x01);
    /* Shift the last packet one slot later, dropping a CC into the gap. */
    memcpy(&buf[slot], &buf[slot - 4], 4);
    buf[slot - 4] = 0x0B; buf[slot - 3] = 0xB0; buf[slot - 2] = 0x4F; buf[slot - 1] = 0x40;
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "interleaved: still parsed");
}

static void test_cable2_full_envelope_ignored(void) {
    /* An external USB-MIDI device (cable 2) emitting the identical Ableton
     * envelope must never be mistaken for Move's own state. */
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    memset(msg, 0, sizeof msg);
    const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x37 };
    memcpy(msg, hdr, sizeof hdr);
    msg[7] = XMOS_AUDIO_KEY_OUT_SRC;
    msg[8] = 0x01;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_raw_msg(buf, &slot, 2, msg);

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "cable2: full envelope on cable 2 ignored");
    CHECK(st.usbc_out == -1, "cable2: usbc_out untouched");
}

static void test_cable2_interleaved_does_not_break_detection(void) {
    /* A cable-2 sysex packet lands mid-message on cable 0; it must be
     * skipped without disturbing the in-progress cable-0 reassembly. */
    uint8_t packets[XMOS_AUDIO_PACKETS * 4];
    build_packets(packets, XMOS_AUDIO_KEY_OUT_SRC, 0x01);

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int pos = 0;
    memcpy(&buf[pos], packets, 3 * 4); pos += 3 * 4;
    buf[pos] = 0x24; buf[pos + 1] = 0x11; buf[pos + 2] = 0x22; buf[pos + 3] = 0x33;
    pos += 4;
    memcpy(&buf[pos], packets + 3 * 4, 5 * 4); pos += 5 * 4;

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 1, "cable2 interleaved: still detected");
    CHECK(st.usbc_out == 1, "cable2 interleaved: usbc_out resolved");
}

static void test_reassembly_across_frame_boundary(void) {
    /* Move's own capture shows the pair occupying 16 of 20 MIDI_OUT slots,
     * so concurrent LED/CC traffic can force a message to split across two
     * SPI frames (two scan() calls). */
    uint8_t packets[XMOS_AUDIO_PACKETS * 4];
    build_packets(packets, XMOS_AUDIO_KEY_OUT_SRC, 0x01);

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;

    uint8_t frame1[MIDI_OUT_LEN];
    memset(frame1, 0, sizeof frame1);
    memcpy(frame1, packets, 5 * 4);
    int changed1 = xmos_audio_scan(frame1, MIDI_OUT_LEN, &st);
    CHECK(changed1 == 0, "split frame: no change on incomplete message");
    CHECK(st.usbc_out == -1, "split frame: usbc_out still unknown mid-message");

    uint8_t frame2[MIDI_OUT_LEN];
    memset(frame2, 0, sizeof frame2);
    memcpy(frame2, packets + 5 * 4, 3 * 4);
    int changed2 = xmos_audio_scan(frame2, MIDI_OUT_LEN, &st);
    CHECK(changed2 == 1, "split frame: change detected once continuation arrives");
    CHECK(st.usbc_out == 1, "split frame: usbc_out resolved after continuation");
}

static void test_route_without_out_src_message(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    int slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x03);
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 0, "route only: no change reported");
    CHECK(st.usbc_out == -1, "route only: usbc_out remains unknown");
    CHECK(st.have_route == 1, "route only: route captured");
    CHECK(st.route[8] == 0x03, "route only: route payload byte captured");
}

static void test_state_init_macro_detects_mic_as_change(void) {
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    CHECK(st.usbc_out == -1, "init macro: usbc_out starts unknown");
    uint8_t buf[MIDI_OUT_LEN];
    make_frame(buf, 0x00, 0x00);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(changed == 1, "init macro: mic selection reported as change");
    CHECK(st.usbc_out == 0, "init macro: usbc_out == 0 after mic scan");
    CHECK(st.seq == 1, "init macro: seq bumped");
}

static void test_build_preserves_input_route_bit(void) {
    uint8_t buf[MIDI_OUT_LEN];
    /* Move last selected USB-C *input* (bit0) with monitoring off. */
    make_frame(buf, 0x01, 0x00);
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);

    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[8] == 0x03, "build: monitor bit set, input route bit kept");
    CHECK(mon[7] == XMOS_AUDIO_KEY_OUT_SRC && mon[8] == 0x01, "build: out-src message correct");

    xmos_audio_build(&st, 0, route, mon);
    CHECK(route[8] == 0x01, "build: monitor bit cleared, input route bit kept");
    CHECK(mon[8] == 0x00, "build: out-src message cleared");
}

static void test_build_without_observation(void) {
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(route[0] == 0xF0 && route[6] == 0x37, "build cold: header present");
    CHECK(route[7] == XMOS_AUDIO_KEY_ROUTE, "build cold: route key");
    CHECK(route[8] == 0x02, "build cold: only monitor bit set");
    CHECK(route[XMOS_AUDIO_MSG_LEN - 1] == 0xF7, "build cold: terminated");
}

static void test_build_idempotent_when_monitor_bit_already_set(void) {
    uint8_t buf[MIDI_OUT_LEN];
    /* Input-route bit set AND monitor already set from a prior observation. */
    make_frame(buf, 0x03, 0x01);
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);

    uint8_t route1[XMOS_AUDIO_MSG_LEN], mon1[XMOS_AUDIO_MSG_LEN];
    uint8_t route2[XMOS_AUDIO_MSG_LEN], mon2[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route1, mon1);
    xmos_audio_build(&st, 1, route2, mon2);
    CHECK(memcmp(route1, route2, XMOS_AUDIO_MSG_LEN) == 0, "build idempotent: route bytes stable");
    CHECK(route1[8] == 0x03, "build idempotent: already-set monitor bit stays set");
}

static void test_emit_round_trip(void) {
    xmos_audio_state_t src = XMOS_AUDIO_STATE_INIT;
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&src, 1, route, mon);

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit: route accepted");
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, mon) == 1, "emit: mon accepted");

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
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

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
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

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 0, "emit: refuses when short on slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: buffer untouched on refusal");
}

static void test_emit_refuses_when_free_slots_not_contiguous(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* Occupy every other slot: plenty of free slots overall (10 of 20), but
     * no run of 8 contiguous ones. */
    for (int i = 0; i + 4 <= MIDI_OUT_LEN; i += 8) {
        buf[i] = 0x0B; buf[i+1] = 0xB0; buf[i+2] = 0x10; buf[i+3] = 0x7F;
    }
    uint8_t snapshot[MIDI_OUT_LEN];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 0, "emit: refuses fragmented free slots");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: buffer untouched when fragmented");
}

static void test_emit_refuses_when_unterminated_sysex_present(void) {
    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    /* An in-flight cable-0 SysEx start packet with no terminator yet — its
     * continuation is presumably arriving next frame. Plenty of free slots
     * elsewhere, but the whole frame must be gated. */
    buf[0] = 0x04; buf[1] = 0xF0; buf[2] = 0x00; buf[3] = 0x21;
    uint8_t snapshot[MIDI_OUT_LEN];
    memcpy(snapshot, buf, sizeof snapshot);

    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 0, "emit: refuses when unterminated sysex present");
    CHECK(memcmp(buf, snapshot, sizeof snapshot) == 0, "emit: buffer untouched when gated");
}

static void test_emit_byte_exact_against_capture(void) {
    /* Literal expected bytes, independent of emit()'s own framing logic and
     * of put_raw_msg()/put_msg() above — pins the real captured framing:
     * 7 packets of CIN 0x04 then one CIN 0x06, all cable 0. */
    static const uint8_t expected[XMOS_AUDIO_PACKETS * 4] = {
        0x04, 0xf0, 0x00, 0x21,
        0x04, 0x1d, 0x01, 0x01,
        0x04, 0x37, 0x12, 0x02,
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x06, 0x00, 0xf7, 0x00,
    };
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;  /* cold: no observed route */
    uint8_t route[XMOS_AUDIO_MSG_LEN], mon[XMOS_AUDIO_MSG_LEN];
    xmos_audio_build(&st, 1, route, mon);

    uint8_t buf[MIDI_OUT_LEN];
    memset(buf, 0, sizeof buf);
    CHECK(xmos_audio_emit(buf, MIDI_OUT_LEN, route) == 1, "emit byte-exact: accepted");
    CHECK(memcmp(buf, expected, sizeof expected) == 0, "emit byte-exact: matches captured framing");
}

/* Bit1 of 37 12 is monitoring — the mechanism that actually routes Main Out to
 * USB-C — so it has to be tracked in its own right, not inferred from 37 14. */
static void test_monitor_bit_tracked_from_route(void) {
    uint8_t buf[MIDI_OUT_LEN];
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int slot;

    CHECK(st.monitor == -1, "monitor: starts unknown, not 0");

    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x02);   /* monitor on */
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.monitor == 1, "monitor: 37 12 02 sets monitor");

    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x00);   /* monitor off */
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.monitor == 0, "monitor: 37 12 00 clears monitor");

    /* bit0 set, bit1 clear — the exact byte Move's sampling page sent. */
    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x01);
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.monitor == 0, "monitor: bit0 alone does not read as monitoring");

    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x03);   /* both bits */
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.monitor == 1, "monitor: bit1 read independently of bit0");
}

/* The hardware capture, 2026-08-26: changing the sampling source emits a LONE
 * 37 12 with bit1 clear. usbc_out must not move (nothing said anything about
 * the out source) while monitor must — that difference is the whole signal. */
static void test_lone_route_clears_monitor_without_touching_usbc_out(void) {
    uint8_t buf[MIDI_OUT_LEN];
    xmos_audio_state_t st = XMOS_AUDIO_STATE_INIT;
    int slot;

    /* Establish Main Out the way the wire does: 37 12 02 then 37 14 01. */
    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x02);
    put_msg(buf, &slot, XMOS_AUDIO_KEY_OUT_SRC, 0x01);
    xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1 && st.monitor == 1, "lone: Main Out established");

    /* Sampling page selects the USB-C input: 37 12 01, no 37 14. */
    memset(buf, 0, sizeof buf); slot = 0;
    put_msg(buf, &slot, XMOS_AUDIO_KEY_ROUTE, 0x01);
    int changed = xmos_audio_scan(buf, MIDI_OUT_LEN, &st);
    CHECK(st.usbc_out == 1, "lone: a lone 37 12 must not move usbc_out");
    CHECK(st.monitor == 0, "lone: a lone 37 12 DOES clear monitoring");
    CHECK(changed == 0, "lone: no usbc_out change is reported");
}

int main(void) {
    test_main_out();
    test_mic();
    test_idempotent();
    test_transition_main_out_to_mic();
    test_ignores_led_sysex();
    test_rejects_truncated();
    test_interleaved_non_sysex();
    test_cable2_full_envelope_ignored();
    test_cable2_interleaved_does_not_break_detection();
    test_reassembly_across_frame_boundary();
    test_route_without_out_src_message();
    test_state_init_macro_detects_mic_as_change();
    test_build_preserves_input_route_bit();
    test_build_without_observation();
    test_build_idempotent_when_monitor_bit_already_set();
    test_emit_round_trip();
    test_emit_respects_occupied_slots();
    test_emit_defers_when_full();
    test_emit_refuses_when_free_slots_not_contiguous();
    test_emit_refuses_when_unterminated_sysex_present();
    test_emit_byte_exact_against_capture();
    test_monitor_bit_tracked_from_route();
    test_lone_route_clears_monitor_without_touching_usbc_out();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
