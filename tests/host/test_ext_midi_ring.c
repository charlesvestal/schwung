/*
 * Does an overtake unload take the chain's knob CCs with it?
 *
 * The ROUTE_EXTERNAL ring grew a second producer when chain knob CC out landed
 * (PR #307). Overtake unload discards the departing DSP's queued packets, and
 * that discard used to reset head/tail — correct while the overtake DSP was
 * the only producer, wrong the moment the chain became one, because the chain
 * is still running and its packets are interleaved with the DSP's.
 *
 * Losing a chain packet HERE is worse than the ring being full. A full ring
 * returns 0 and knob_emit_cc_out declines to record the value, so it retries.
 * A packet accepted and then blanked was recorded as delivered, and that knob's
 * motor stays wrong until the user happens to move it again.
 *
 * The properties worth proving are the ones that are silent when wrong:
 *   1. a discard removes ONLY the named source;
 *   2. it does NOT reset the cursors (which is how it would take the other
 *      producer's packets);
 *   3. the drain steps OVER a discarded slot without spending a mailbox slot,
 *      so a discard costs no throughput;
 *   4. FIFO order across the two producers survives all of it;
 *   5. a packet that does not fit the block STAYS in the ring — a delay, not a
 *      loss — because that is the promise knob_emit_cc_out's retry relies on.
 */
#include <stdio.h>
#include <string.h>

#include "ext_midi_ring.h"
#include "shadow_constants.h"   /* HW_MIDI_OUT_SIZE — the real 80 */

/* The mailbox MIDI_OUT region, as the shim passes it. Taken from the real
 * constant rather than written as 80 here: a test that hard-codes the size
 * still passes after the region moves, which is the case this ring would
 * quietly overrun. */
#define HW_MIDI_OUT_REGION HW_MIDI_OUT_SIZE

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static uint8_t pkt_of(uint8_t d1) { return d1; }

static void push(ext_midi_ring_t *r, uint8_t d1, uint8_t src, int expect) {
    const uint8_t msg[4] = { 0x2B, 0xB0, pkt_of(d1), 64 };
    int rc = ext_midi_ring_push(r, msg, 4, src);
    if (rc != expect) {
        printf("  FAIL push d1=%u src=%u returned %d, expected %d\n",
               d1, src, rc, expect);
        failures++;
    }
}

int main(void) {
    ext_midi_ring_t r;
    uint8_t out[HW_MIDI_OUT_REGION];

    /* 1. A DISCARD REMOVES ONLY THE NAMED SOURCE. */
    printf("an overtake unload leaves the chain's packets alone\n");
    ext_midi_ring_init(&r);
    memset(out, 0, sizeof(out));
    push(&r, 10, EXT_SRC_OVERTAKE, 4);
    push(&r, 11, EXT_SRC_CHAIN,    4);
    push(&r, 12, EXT_SRC_OVERTAKE, 4);
    push(&r, 13, EXT_SRC_CHAIN,    4);

    uint32_t head_before = r.head, tail_before = r.tail;
    ext_midi_ring_discard_source(&r, EXT_SRC_OVERTAKE);
    check(r.head == head_before && r.tail == tail_before,
          "the discard does not touch the cursors");

    int n = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n == 2, "only the two chain packets are drained");
    check(out[2] == 11 && out[6] == 13,
          "and they are the chain's, in order");
    check(out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 0,
          "a discarded packet spends no mailbox slot");

    /* 2. THE MIRROR CASE — discarding the chain must not eat the DSP's. This
     * is not a thing the shim does, and that is exactly why it is here: it
     * proves the filter is on the TAG and not on some incidental ordering. */
    printf("the tag is what selects, not the order\n");
    ext_midi_ring_init(&r);
    memset(out, 0, sizeof(out));
    push(&r, 20, EXT_SRC_OVERTAKE, 4);
    push(&r, 21, EXT_SRC_CHAIN,    4);
    ext_midi_ring_discard_source(&r, EXT_SRC_CHAIN);
    n = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n == 1 && out[2] == 20, "discarding the chain leaves the DSP's packet");

    /* 3. FIFO ACROSS PRODUCERS. */
    printf("one queue, one order\n");
    ext_midi_ring_init(&r);
    memset(out, 0, sizeof(out));
    for (int i = 0; i < 8; i++)
        push(&r, (uint8_t)(30 + i), (i % 2) ? EXT_SRC_CHAIN : EXT_SRC_OVERTAKE, 4);
    n = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n == 8, "all eight drain in one block");
    int ordered = 1;
    for (int i = 0; i < 8; i++)
        if (out[i * 4 + 2] != (uint8_t)(30 + i)) ordered = 0;
    check(ordered, "interleaved producers keep insertion order");

    /* 4. A PACKET THAT DOES NOT FIT IS DELAYED, NOT LOST. */
    printf("an over-full block delays, it does not drop\n");
    ext_midi_ring_init(&r);
    memset(out, 0, sizeof(out));
    for (int i = 0; i < 30; i++)
        push(&r, (uint8_t)(40 + i), EXT_SRC_CHAIN, 4);
    n = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n == HW_MIDI_OUT_REGION / 4, "the block fills exactly");
    check(n < 30, "and it could not take them all");
    memset(out, 0, sizeof(out));
    int n2 = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n + n2 == 30, "the remainder arrives on the next block");
    check(out[2] == (uint8_t)(40 + n),
          "and it resumes exactly where the last block stopped");

    /* 5. PRE-OCCUPIED SLOTS ARE STEPPED OVER, NOT OVERWRITTEN. Move's own
     * output is already in this region in normal shadow mode — this ring was
     * designed and measured under overtake, where the region is cleared first. */
    printf("Move's own MIDI_OUT is not overwritten\n");
    ext_midi_ring_init(&r);
    memset(out, 0, sizeof(out));
    out[0] = 0x09; out[1] = 0x90; out[2] = 60; out[3] = 100;   /* Move's packet */
    push(&r, 50, EXT_SRC_CHAIN, 4);
    n = ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(n == 1, "one packet placed");
    check(out[2] == 60, "Move's packet is untouched");
    check(out[6] == 50, "ours went into the next free slot");

    /* 6. FULL RING RETURNS 0, AND 0 MUST MEAN NOT SENT. */
    printf("a full ring refuses rather than overwriting\n");
    ext_midi_ring_init(&r);
    for (int i = 0; i < EXT_MIDI_RING_PACKETS; i++)
        push(&r, (uint8_t)i, EXT_SRC_CHAIN, 4);
    push(&r, 99, EXT_SRC_CHAIN, 0);
    check(r.drops == 1, "the drop is counted");
    memset(out, 0, sizeof(out));
    (void)ext_midi_ring_drain(&r, out, (int)sizeof(out));
    check(out[2] == 0, "drop-NEWEST: the oldest packet is still first");

    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("PASS: ext midi ring — a discard is scoped to one producer\n");
    return 0;
}
