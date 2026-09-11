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

/*
 * NOTE: the drain is PACED at UI_MIDI_CARRY_PACKETS_PER_FRAME packets per
 * frame. It used to fill every free slot in the mailbox, which sent a long
 * SysEx at ~6900 packets/s -- measured on hardware 2026-09-10, a 394-packet
 * message arrived with most of its middle missing, because the loss on that
 * path is rate dependent (docs/SYSEX.md: 31 packets alone byte-perfect, 34
 * amid traffic losing 8). These expectations therefore count the CAP, not the
 * free space, wherever the two differ.
 */

static void test_fits_in_one_frame(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t region[REGION]; memset(region, 0, sizeof(region));

    for (int i = 0; i < 5; i++) {
        uint8_t pkt[4] = { 0x24, (uint8_t)i, 0x11, 0x22 };
        CHECK(ui_midi_carry_push(&c, pkt), "small push accepted");
    }
    int placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed == UI_MIDI_CARRY_PACKETS_PER_FRAME,
          "the per-frame cap is placed, not everything that fits");
    CHECK(c.len == (5 - UI_MIDI_CARRY_PACKETS_PER_FRAME) * 4,
          "the remainder waits for the next frame");
    CHECK(region_used(region) == UI_MIDI_CARRY_PACKETS_PER_FRAME, "the cap decides how many slots are used");
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
    CHECK(placed == UI_MIDI_CARRY_PACKETS_PER_FRAME,
          "one frame places the per-frame cap");
    /* THE REGRESSION. The old code discarded these. */
    CHECK(c.len == (53 - UI_MIDI_CARRY_PACKETS_PER_FRAME) * 4,
          "the remainder is HELD, not discarded");
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
    /* Enough frames for the paced rate: ceil(53/cap) plus slack. */
    while (c.len > 0 && frames < 53) {
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

    CHECK(frames == (53 + UI_MIDI_CARRY_PACKETS_PER_FRAME - 1) / UI_MIDI_CARRY_PACKETS_PER_FRAME,
          "53 packets take ceil(53/cap) frames");
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
    CHECK(c.buf[1] == UI_MIDI_CARRY_PACKETS_PER_FRAME,
          "head of the carry is the first UNPLACED packet, not packet 0");

    /* Append after the partial drain, then finish. New work must land BEHIND. */
    uint8_t late[4] = { 0x24, 0xEE, 0, 0 };
    ui_midi_carry_push(&c, late);
    memset(region, 0, sizeof(region));
    ui_midi_carry_drain(&c, region, REGION);
    CHECK(region[1] == UI_MIDI_CARRY_PACKETS_PER_FRAME, "next frame resumes where the last stopped");

    /* Drain to empty and check ORDER rather than a fixed slot: with a paced
     * drain the late packet's position depends on the cap, but the property
     * that matters is that new work lands BEHIND what was already queued.
     * Asserting a slot index would pin the rate; asserting the order pins the
     * invariant. */
    uint8_t last_seen = 0;
    int guard = 0;
    while (c.len > 0 && guard++ < 64) {
        memset(region, 0, sizeof(region));
        ui_midi_carry_drain(&c, region, REGION);
        for (int i = 0; i < REGION; i += 4)
            if (region[i]) last_seen = region[i + 1];
    }
    CHECK(last_seen == 0xEE, "the late packet is last, not first");
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
    CHECK(placed == (10 < UI_MIDI_CARRY_PACKETS_PER_FRAME ? 10 : UI_MIDI_CARRY_PACKETS_PER_FRAME),
          "free space and the cap both bound a frame, whichever is smaller");
    CHECK(c.len == (30 - (10 < UI_MIDI_CARRY_PACKETS_PER_FRAME ? 10 : UI_MIDI_CARRY_PACKETS_PER_FRAME)) * 4,
          "the rest is held for the next frame");
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


/* ===========================================================================
 * INTERLEAVE: quiet-start and the collision retry.
 *
 * Only System Realtime bytes may appear inside a SysEx, so a Note On spliced
 * into one makes a conformant receiver abort the whole message. Isolated on
 * hardware 2026-09-11 with the transport STOPPED both ways: playing notes with
 * Move's MIDI out ON garbles the E16, MIDI out OFF is clean -- while 559
 * packets/sec with no foreign traffic stayed perfectly clean, which is what
 * rules out the rate explanation the bug wore for three sessions.
 * ======================================================================== */

/* A note-on from Move, on cable 2 -- the packet that does the damage. */
static void put_foreign(uint8_t *region, int slot)
{
    region[slot*4 + 0] = 0x29;   /* cable 2, CIN 9 = note-on */
    region[slot*4 + 1] = 0x90;
    region[slot*4 + 2] = 60;
    region[slot*4 + 3] = 100;
}

static void test_quiet_start_defers_into_a_dirty_mailbox(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)i;
    int n = packetize(msg, 24, pkts);
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    uint8_t region[REGION] = {0};
    put_foreign(region, 0);

    int placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed == 0,
          "quiet-start: a run must not OPEN into a mailbox that already holds "
          "foreign cable-2 traffic -- starting there splices the note in");
    CHECK(region_used(region) == 1,
          "quiet-start: nothing of ours was placed, only Move's own packet is there");

    /* Next frame is clear, so it goes. */
    memset(region, 0, REGION);
    placed = ui_midi_carry_drain(&c, region, REGION);
    CHECK(placed > 0, "quiet-start DEFERS, it does not drop -- the clear frame sends");
}

static void test_quiet_start_gives_up_rather_than_starving(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)i;
    int n = packetize(msg, 24, pkts);
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    /* A mailbox that is never clear. The screen must still update: the
     * quiet-start is an optimisation, never a precondition. */
    int total = 0;
    for (int f = 0; f < UI_MIDI_CARRY_START_DEFER_MAX + 2; f++) {
        uint8_t region[REGION] = {0};
        put_foreign(region, 0);
        total += ui_midi_carry_drain(&c, region, REGION);
    }
    CHECK(total > 0,
          "a dense note stream must not be able to stop the screen updating "
          "ALTOGETHER -- the defer is capped and we go anyway");
}

/*
 * The retry is OFF by default (UI_MIDI_CARRY_MSG_RETRIES == 0) because at this
 * link's collision rate it made the garbling WORSE -- see the constant. These
 * two tests drive the machinery at a non-zero cap so it stays correct for
 * whoever turns it on, and are skipped when the cap is 0 rather than deleted.
 */
#if UI_MIDI_CARRY_MSG_RETRIES > 0
static void test_collided_run_is_requeued_whole(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    const int before = ui_midi_carry_retry_count();

    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)(i + 1);
    int n = packetize(msg, 24, pkts);          /* 8 packets */
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    /* Frame 1: clear, opens the run and places `pace` packets. */
    uint8_t region[REGION] = {0};
    ui_midi_carry_drain(&c, region, REGION);
    CHECK(c.msg_len > 0, "the run is open after the first frame");

    /* Frame 2: Move drops a note in, mid-run. */
    memset(region, 0, REGION);
    put_foreign(region, 0);
    ui_midi_carry_drain(&c, region, REGION);

    /* Drain on clear frames until the run closes and the retry is appended --
     * then STOP, or the next frames start draining the re-queued copy and the
     * carry no longer holds the thing under test. */
    for (int f = 0; f < 8; f++) {
        if (ui_midi_carry_retry_count() != before) break;
        memset(region, 0, REGION);
        ui_midi_carry_drain(&c, region, REGION);
    }

    CHECK(ui_midi_carry_retry_count() == before + 1,
          "a run with a foreign packet inside it is re-queued exactly once");
    CHECK(c.len == n * 4,
          "the re-queue is the WHOLE message -- a partial resend is another "
          "truncated SysEx, which is the fault being repaired");

    /* And it is byte-identical to what went in. */
    CHECK(memcmp(c.buf, pkts, (size_t)(n * 4)) == 0,
          "the re-queued copy is byte-for-byte the original run");
}

#endif /* UI_MIDI_CARRY_MSG_RETRIES > 0 */

/* Valid at ANY cap, including 0: a clean run is never resent. */
static void test_clean_run_is_not_requeued(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    const int before = ui_midi_carry_retry_count();

    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)i;
    int n = packetize(msg, 24, pkts);
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    for (int f = 0; f < 12 && c.len > 0; f++) {
        uint8_t region[REGION] = {0};
        ui_midi_carry_drain(&c, region, REGION);
    }
    CHECK(ui_midi_carry_retry_count() == before,
          "A CLEAN RUN IS NEVER RESENT -- retrying unconditionally would double "
          "every message on the wire and make the rate problem real");
    CHECK(c.len == 0, "and the carry is empty afterwards");
}

#if UI_MIDI_CARRY_MSG_RETRIES > 0
static void test_retry_is_capped(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    const int before = ui_midi_carry_retry_count();

    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)i;
    int n = packetize(msg, 24, pkts);
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    /* Every frame collides. Without a cap this re-queues forever. */
    for (int f = 0; f < 400 && c.len > 0; f++) {
        uint8_t region[REGION] = {0};
        put_foreign(region, 19);   /* last slot: ours still get placed */
        ui_midi_carry_drain(&c, region, REGION);
    }

    CHECK(ui_midi_carry_retry_count() - before <= UI_MIDI_CARRY_MSG_RETRIES,
          "the retry is CAPPED -- under continuous playing every attempt can "
          "collide, and a message that re-queues itself forever starves the "
          "next real update");
    CHECK(c.drops == 0,
          "A RETRY MUST NEVER COST A DROP -- repairing a garble by dropping a "
          "packet is the identical fault one buffer along");
}
#endif /* UI_MIDI_CARRY_MSG_RETRIES > 0 */

/*
 * THE DEFAULT MUST BE OFF, and this is the pin that says so.
 *
 * Turning the retry back on without measuring the collision rate again is the
 * mistake that produced "now even worse"; the constant carries the reasoning
 * and this fails if somebody flips it without reading it.
 */
static void test_retry_is_off_by_default(void)
{
    ui_midi_carry_t c; ui_midi_carry_reset(&c);
    const int before = ui_midi_carry_retry_count();

    uint8_t msg[24], pkts[64];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)i;
    int n = packetize(msg, 24, pkts);
    for (int i = 0; i < n; i++) ui_midi_carry_push(&c, &pkts[i*4]);

    /* Collide every single frame. */
    for (int f = 0; f < 60 && c.len > 0; f++) {
        uint8_t region[REGION] = {0};
        put_foreign(region, 19);
        ui_midi_carry_drain(&c, region, REGION);
    }

    CHECK(UI_MIDI_CARRY_MSG_RETRIES == 0,
          "the retry ships OFF -- it AMPLIFIES at this link's collision rate, "
          "and at idle the heartbeat is the only traffic there is, so tripling "
          "it triples the corrupt messages reaching the screen");
    CHECK(ui_midi_carry_retry_count() == before,
          "with the cap at 0 a collided run is never re-queued");
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
    test_quiet_start_defers_into_a_dirty_mailbox();
    test_quiet_start_gives_up_rather_than_starving();
#if UI_MIDI_CARRY_MSG_RETRIES > 0
    test_collided_run_is_requeued_whole();
    test_retry_is_capped();
#endif
    test_clean_run_is_not_requeued();
    test_retry_is_off_by_default();

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("PASS: ui_midi_out_carry\n");
    return 0;
}
