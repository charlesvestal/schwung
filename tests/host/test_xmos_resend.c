/* The XMOS control-message re-send, replayed against the captured failure.
 *
 * The scenario in test_the_captured_failure() is not invented: the frame
 * numbers, the surviving pair, and the RGB LED SysEx that replaced it are taken
 * from a hardware capture on 2026-09-12 (f13596, the Main Out selection that
 * silently did nothing). See docs/DIAGNOSTICS.md.
 */
#include <stdio.h>
#include <string.h>
#include "xmos_resend.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s\n", (what)); failures++; } \
} while (0)

/* ---- builders -------------------------------------------------------------
 * A 23-byte Ableton envelope, packed into USB-MIDI SysEx packets exactly as
 * Move does it: seven cin-0x04 packets of three bytes, then cin 0x06 with the
 * last payload byte and F7.
 */
static void envelope(uint8_t msg[XMOS_AUDIO_MSG_LEN], uint8_t key, uint8_t val)
{
    memset(msg, 0, XMOS_AUDIO_MSG_LEN);
    msg[0] = 0xF0; msg[1] = 0x00; msg[2] = 0x21; msg[3] = 0x1D;
    msg[4] = 0x01; msg[5] = 0x01; msg[6] = 0x37; msg[7] = key; msg[8] = val;
    msg[XMOS_AUDIO_MSG_LEN - 1] = 0xF7;
}

/* Pack `msg` into midi_out starting at byte offset `at`. Returns bytes used. */
static int pack(uint8_t *midi_out, int at, const uint8_t *msg)
{
    int slot = at;
    int i = 0;
    while (i < XMOS_AUDIO_MSG_LEN) {
        int left = XMOS_AUDIO_MSG_LEN - i;
        if (left >= 3 && !(left == 3 && i + 3 == XMOS_AUDIO_MSG_LEN)) {
            midi_out[slot] = 0x04;
            midi_out[slot + 1] = msg[i];
            midi_out[slot + 2] = msg[i + 1];
            midi_out[slot + 3] = msg[i + 2];
            i += 3;
        } else if (left == 3) {
            midi_out[slot] = 0x07;
            midi_out[slot + 1] = msg[i];
            midi_out[slot + 2] = msg[i + 1];
            midi_out[slot + 3] = msg[i + 2];
            i += 3;
        } else if (left == 2) {
            midi_out[slot] = 0x06;
            midi_out[slot + 1] = msg[i];
            midi_out[slot + 2] = msg[i + 1];
            midi_out[slot + 3] = 0;
            i += 2;
        } else {
            midi_out[slot] = 0x05;
            midi_out[slot + 1] = msg[i];
            midi_out[slot + 2] = 0;
            midi_out[slot + 3] = 0;
            i += 1;
        }
        slot += 4;
    }
    return slot - at;
}

/* The RGB LED SysEx that was found in the mailbox instead, verbatim from the
 * capture: 04 f0 00 21 / 04 1d 01 01 / 04 3b 10 4a / 04 0f 00 0f /
 * 04 00 0f 00 / 05 f7 00 00 */
static void pack_led_sysex(uint8_t *midi_out)
{
    static const uint8_t pkts[6][4] = {
        {0x04, 0xF0, 0x00, 0x21}, {0x04, 0x1D, 0x01, 0x01},
        {0x04, 0x3B, 0x10, 0x4A}, {0x04, 0x0F, 0x00, 0x0F},
        {0x04, 0x00, 0x0F, 0x00}, {0x05, 0xF7, 0x00, 0x00},
    };
    for (int p = 0; p < 6; p++) memcpy(midi_out + p * 4, pkts[p], 4);
}

/* ---- tests --------------------------------------------------------------- */

static void test_scan_roundtrip(void)
{
    uint8_t midi_out[80] = {0};
    uint8_t route[XMOS_AUDIO_MSG_LEN], src[XMOS_AUDIO_MSG_LEN];
    envelope(route, 0x12, 0x02);
    envelope(src, 0x14, 0x01);
    int used = pack(midi_out, 0, route);
    pack(midi_out, used, src);

    uint8_t got[XMOS_RESEND_MAX_MSGS][XMOS_AUDIO_MSG_LEN];
    int n = xmos_resend_scan(midi_out, 80, got, XMOS_RESEND_MAX_MSGS);
    CHECK(n == 2, "scan: the pair is two envelopes");
    CHECK(n == 2 && xmos_resend_same(got[0], route), "scan: first is 37 12 02");
    CHECK(n == 2 && xmos_resend_same(got[1], src), "scan: second is 37 14 01");
    CHECK(xmos_resend_present(midi_out, 80, route), "present: finds 37 12");
    CHECK(xmos_resend_present(midi_out, 80, src), "present: finds 37 14");
}

static void test_led_sysex_is_not_a_37(void)
{
    uint8_t midi_out[80] = {0};
    pack_led_sysex(midi_out);
    uint8_t got[XMOS_RESEND_MAX_MSGS][XMOS_AUDIO_MSG_LEN];
    /* The LED command is a complete Ableton SysEx on the same cable. It must
     * not be mistaken for something to watch — otherwise every LED repaint
     * becomes a watched message and the budget is spent on Move's own LEDs. */
    CHECK(xmos_resend_scan(midi_out, 80, got, XMOS_RESEND_MAX_MSGS) == 0,
          "an RGB LED SysEx (3b) is not a 37-family envelope");
}

static void test_survivor_is_left_alone(void)
{
    xmos_resend_t st = XMOS_RESEND_INIT;
    uint8_t shadow[80] = {0}, hw[80] = {0};
    uint8_t route[XMOS_AUDIO_MSG_LEN], src[XMOS_AUDIO_MSG_LEN];
    envelope(route, 0x12, 0x02);
    envelope(src, 0x14, 0x01);
    int used = pack(shadow, 0, route);
    pack(shadow, used, src);
    memcpy(hw, shadow, sizeof(hw));           /* it reached the wire */

    xmos_resend_observe(&st, shadow, 80);
    xmos_resend_confirm(&st, hw, 80);

    uint8_t out[XMOS_AUDIO_MSG_LEN];
    CHECK(xmos_resend_take(&st, out) == 0, "a delivered message is not re-sent");
    CHECK(st.delivered == 2, "both halves counted delivered");
    CHECK(st.lost == 0 && st.resent == 0, "nothing lost, nothing re-sent");
    CHECK(!xmos_resend_has_work(&st), "no work left");
}

static void test_the_captured_failure(void)
{
    xmos_resend_t st = XMOS_RESEND_INIT;
    uint8_t shadow[80] = {0}, hw[80] = {0};
    uint8_t route[XMOS_AUDIO_MSG_LEN], src[XMOS_AUDIO_MSG_LEN];
    envelope(route, 0x12, 0x02);
    envelope(src, 0x14, 0x01);
    int used = pack(shadow, 0, route);
    pack(shadow, used, src);
    pack_led_sysex(hw);                       /* what actually went out */

    xmos_resend_observe(&st, shadow, 80);
    xmos_resend_confirm(&st, hw, 80);

    CHECK(st.lost == 2, "both halves of the pair detected lost");
    CHECK(xmos_resend_has_work(&st), "a re-send is queued");

    uint8_t a[XMOS_AUDIO_MSG_LEN], b[XMOS_AUDIO_MSG_LEN], c[XMOS_AUDIO_MSG_LEN];
    CHECK(xmos_resend_take(&st, a) == 1, "first re-send handed over");
    CHECK(xmos_resend_take(&st, b) == 1, "second re-send handed over");
    CHECK(xmos_resend_take(&st, c) == 0, "only two — one per message, not a flood");
    /* Both halves come back, in either slot order; the pair is what matters. */
    int ok = (xmos_resend_same(a, route) && xmos_resend_same(b, src)) ||
             (xmos_resend_same(a, src) && xmos_resend_same(b, route));
    CHECK(ok, "the re-sends are Move's own bytes, verbatim");

    /* Our re-send lands. Confirmation asks whether the bytes are on the wire,
     * not who put them there, so this closes the watch. */
    uint8_t hw2[80] = {0};
    used = pack(hw2, 0, route);
    pack(hw2, used, src);
    xmos_resend_confirm(&st, hw2, 80);
    CHECK(st.delivered == 2, "the re-send is what confirms delivery");
    CHECK(!xmos_resend_has_work(&st), "watch closed after delivery");
    CHECK(st.gave_up == 0, "nobody gave up");
}

static void test_budget_is_bounded(void)
{
    xmos_resend_t st = XMOS_RESEND_INIT;
    uint8_t shadow[80] = {0}, hw[80] = {0};
    uint8_t route[XMOS_AUDIO_MSG_LEN];
    envelope(route, 0x12, 0x02);
    pack(shadow, 0, route);
    pack_led_sysex(hw);

    /* A mailbox that never carries it. Re-observing our own echo each frame
     * must NOT refresh the budget, or this loops forever on hardware. */
    uint8_t out[XMOS_AUDIO_MSG_LEN];
    int sends = 0;
    for (int frame = 0; frame < 200; frame++) {
        xmos_resend_observe(&st, shadow, 80);
        if (xmos_resend_take(&st, out)) sends++;
        xmos_resend_confirm(&st, hw, 80);
    }
    CHECK(sends == XMOS_RESEND_MAX_ATTEMPTS,
          "re-sends are capped at XMOS_RESEND_MAX_ATTEMPTS, not unbounded");
    CHECK(st.gave_up == 1, "it gives up once, and says so");
    if (sends != XMOS_RESEND_MAX_ATTEMPTS)
        printf("      (sent %d times over 200 frames)\n", sends);
}

static void test_new_selection_supersedes(void)
{
    xmos_resend_t st = XMOS_RESEND_INIT;
    uint8_t mic[XMOS_AUDIO_MSG_LEN], main_out[XMOS_AUDIO_MSG_LEN];
    envelope(mic, 0x14, 0x00);
    envelope(main_out, 0x14, 0x01);

    uint8_t shadow[80] = {0}, hw[80] = {0};
    pack(shadow, 0, mic);
    pack_led_sysex(hw);
    xmos_resend_observe(&st, shadow, 80);
    xmos_resend_confirm(&st, hw, 80);
    CHECK(xmos_resend_has_work(&st), "the Mic selection is queued");

    /* The user changes their mind before the re-send goes out. Both are
     * watched — two slots — so the earlier one is not silently dropped, but
     * neither is it re-sent in place of the newer one. */
    memset(shadow, 0, sizeof(shadow));
    pack(shadow, 0, main_out);
    xmos_resend_observe(&st, shadow, 80);

    uint8_t out[XMOS_AUDIO_MSG_LEN];
    int saw_mic = 0, saw_main = 0;
    while (xmos_resend_take(&st, out)) {
        if (xmos_resend_same(out, mic)) saw_mic = 1;
        if (xmos_resend_same(out, main_out)) saw_main = 1;
    }
    CHECK(saw_mic, "the queued Mic re-send still goes (it was genuinely lost)");
    CHECK(!saw_main, "the new selection is watched, not re-sent before it is tried");
}

static void test_nulls(void)
{
    xmos_resend_t st = XMOS_RESEND_INIT;
    uint8_t buf[80] = {0}, out[XMOS_AUDIO_MSG_LEN];
    xmos_resend_observe(NULL, buf, 80);
    xmos_resend_observe(&st, NULL, 80);
    xmos_resend_confirm(NULL, buf, 80);
    xmos_resend_confirm(&st, NULL, 80);
    CHECK(xmos_resend_take(NULL, out) == 0, "take(NULL) is 0");
    CHECK(xmos_resend_take(&st, NULL) == 0, "take(.., NULL) is 0");
    CHECK(xmos_resend_has_work(NULL) == 0, "has_work(NULL) is 0");
    CHECK(xmos_resend_scan(NULL, 80, NULL, 0) == 0, "scan(NULL) is 0");
}

int main(void)
{
    test_scan_roundtrip();
    test_led_sysex_is_not_a_37();
    test_survivor_is_left_alone();
    test_the_captured_failure();
    test_budget_is_bounded();
    test_new_selection_supersedes();
    test_nulls();

    if (failures == 0) {
        printf("test_xmos_resend: PASS\n");
        return 0;
    }
    printf("\n%d failure(s)\n", failures);
    return 1;
}
