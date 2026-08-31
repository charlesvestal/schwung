/*
 * The outbound carry: a packet that does not fit is DELAYED, never destroyed.
 *
 * The defect these pin is not "MIDI_OUT is only 20 packets" — it always was.
 * It is that shadow_inject_ui_midi_out() memset its source before placing
 * anything, so the 21st packet of a flush had nowhere to be. A 158-byte SysEx
 * is 53 packets; it cannot fit in one frame and never could.
 *
 * Every test here therefore checks CONSERVATION across frames, not capacity in
 * one. The load-bearing one is test_large_sysex_survives_three_frames: it
 * reassembles the message from what actually landed in the mailbox and
 * compares it byte-for-byte with what went in. A count alone would pass on a
 * reordered run, which assembles into a well-framed lie.
 */
#include <stdio.h>
#include <string.h>
#include "../../src/host/ui_midi_out_carry.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

#define REGION 80   /* HW_MIDI_OUT_SIZE: 20 packets */

static int region_used(const uint8_t *r)
{
    int n = 0;
    for (int i = 0; i < REGION; i += 4)
        if (r[i] || r[i+1] || r[i+2] || r[i+3]) n++;
    return n;
}

/* Build the USB-MIDI packet stream for a SysEx message, the way a JS module
 * would. Returns packet count. */
static int packetize(const uint8_t *msg, int len, uint8_t *out)
{
    int n = 0, pos = 0;
    while (pos < len) {
        int remain = len - pos;
        uint8_t cin;
        int take;
        if (remain > 3)       { cin = 0x04; take = 3; }
        else if (remain == 3) { cin = 0x07; take = 3; }
        else if (remain == 2) { cin = 0x06; take = 2; }
        else                  { cin = 0x05; take = 1; }
        out[n*4 + 0] = 0x20 | cin;   /* cable 2 */
        out[n*4 + 1] = pos + 0 < len ? msg[pos + 0] : 0;
        out[n*4 + 2] = take > 1 ? msg[pos + 1] : 0;
        out[n*4 + 3] = take > 2 ? msg[pos + 2] : 0;
        pos += take;
        n++;
    }
    return n;
}

static void test_fits_in_one_frame(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t region[REGION]; memset(region, 0, sizeof(region));

    for (int i = 0; i < 5; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0x11, 0x22 };
        CHECK(ui_midi_carry_push(&c, pkt), "small push accepted");
    }
    int placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed == 5, "all five placed in one frame");
    CHECK(c.len == 0, "carry empty after a drain that fit");
    CHECK(region_used(region) == 5, "five slots used");
}

static void test_overflow_is_held_not_dropped(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t region[REGION]; memset(region, 0, sizeof(region));

    /* 53 packets: one 158-byte SysEx, the message from #358. */
    for (int i = 0; i < 53; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0x00, 0x00 };
        ui_midi_carry_push(&c, pkt);
    }
    int placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed == 20, "one frame places exactly the 20 the mailbox holds");
    /* THE REGRESSION. The old code discarded these. */
    CHECK(c.len == (53 - 20) * 4, "the other 33 are HELD, not discarded");
    CHECK(c.drops == 0, "holding is not dropping");
}

static void test_large_sysex_survives_three_frames(void)
{
    /* A 158-byte SysEx with an aligned 00 00 00 run in it, so this also fails
     * if anything downstream reintroduces the #355 zero-payload drop. */
    uint8_t msg[158];
    msg[0] = 0xF0;
    for (int i = 1; i < 157; i++) msg[i] = (uint8_t)(i & 0x7F);
    msg[60] = msg[61] = msg[62] = 0x00;
    msg[157] = 0xF7;

    uint8_t packets[64 * 4];
    int npkt = packetize(msg, sizeof(msg), packets);
    CHECK(npkt == 53, "158-byte SysEx is 53 USB-MIDI packets");

    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    for (int i = 0; i < npkt; i++)
        CHECK(ui_midi_carry_push(&c, &packets[i*4]), "every packet queued");

    /* Drain frame by frame, reassembling from the mailbox as the XMOS would. */
    uint8_t got[256];
    int got_len = 0, frames = 0;
    while (c.len > 0 && frames < 10) {
        uint8_t region[REGION]; memset(region, 0, sizeof(region));
        ui_midi_carry_drain(&c, region, REGION);
        for (int i = 0; i < REGION; i += 4) {
            uint8_t cin = region[i] & 0x0F;
            if (cin < 0x04 || cin > 0x07) continue;
            int take = (cin == 0x05) ? 1 : (cin == 0x06) ? 2 : 3;
            for (int b = 0; b < take && got_len < (int)sizeof(got); b++)
                got[got_len++] = region[i + 1 + b];
        }
        frames++;
    }

    CHECK(frames == 3, "53 packets take three frames at 20 per frame");
    CHECK(got_len == (int)sizeof(msg), "every byte arrived");
    CHECK(memcmp(got, msg, sizeof(msg)) == 0,
          "reassembled message is byte-identical - order preserved");
    CHECK(got[0] == 0xF0 && got[157] == 0xF7, "framing intact");
}

static void test_order_across_a_partial_frame(void)
{
    /* The failure a count-only test cannot see: a partial drain must shift the
     * remainder down, not leave a hole that the next append fills. */
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    for (int i = 0; i < 30; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0, 0 };
        ui_midi_carry_push(&c, pkt);
    }
    uint8_t region[REGION]; memset(region, 0, sizeof(region));
    ui_midi_carry_drain(&c, region, REGION);
    CHECK(c.buf[1] == 20, "head of the carry is packet 20, not packet 0");

    /* Append after the partial drain, then finish. New work must land BEHIND. */
    uint8_t late[4] = { 0x24, 0xEE, 0, 0 };
    ui_midi_carry_push(&c, late);
    memset(region, 0, sizeof(region));
    ui_midi_carry_drain(&c, region, REGION);
    CHECK(region[1] == 20, "next frame resumes at packet 20");
    CHECK(region[10 * 4 + 1] == 0xEE, "the late packet is last, not first");
}

static void test_partially_occupied_region(void)
{
    /* Move's own output and the LED flush share these 80 bytes. */
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t region[REGION]; memset(region, 0, sizeof(region));
    for (int i = 0; i < REGION; i += 8) { region[i] = 0x09; region[i+1] = 0x90; }

    for (int i = 0; i < 30; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0, 0 };
        ui_midi_carry_push(&c, pkt);
    }
    int placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed == 10, "only the ten free slots are used");
    CHECK(c.len == 20 * 4, "the rest is held for the next frame");
    CHECK(region[0] == 0x09, "an occupied slot is never overwritten");
}

static void test_full_carry_drops_newest_and_counts(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    for (int i = 0; i < UI_MIDI_CARRY_PACKETS; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0, 0 };
        CHECK(ui_midi_carry_push(&c, pkt) == 1, "fills to capacity");
    }
    uint8_t over[4] = { 0x24, 0xFF, 0, 0 };
    CHECK(ui_midi_carry_push(&c, over) == 0, "one past capacity is refused");
    CHECK(c.drops == 1, "and counted - the condition has a name now");
    CHECK(c.buf[1] == 0, "drop-NEWEST: the head of the run is untouched");
}

static void test_backpressure_threshold(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    CHECK(ui_midi_carry_wants_more(&c), "an empty carry accepts new work");
    for (int i = 0; i < UI_MIDI_CARRY_PACKETS / 2; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0, 0 };
        ui_midi_carry_push(&c, pkt);
    }
    CHECK(!ui_midi_carry_wants_more(&c),
          "at high water it stops reading, so the SHM buffer fills and JS "
          "sees the false return that already exists");
}

int main(void)
{
    test_fits_in_one_frame();
    test_overflow_is_held_not_dropped();
    test_large_sysex_survives_three_frames();
    test_order_across_a_partial_frame();
    test_partially_occupied_region();
    test_full_carry_drops_newest_and_counts();
    test_backpressure_threshold();

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("PASS: ui_midi_out_carry\n");
    return 0;
}
