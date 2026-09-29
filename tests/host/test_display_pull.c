/* Unit test: the shadow display answers the XMOS's slice REQUEST.
 *
 * shadow_swap_display() used to free-run a 0..6 counter and send whatever slice
 * that counter named, whatever the XMOS had asked for. Snapshots on hardware
 * showed the two sequences out of step, and a slice sent against the wrong
 * request is drawn in the wrong band of the panel: the wrapped screen. The
 * request sequences below are the shape measured there -- the XMOS falling
 * back to 1 or 2 mid-frame -- not a tidy 1..6 cycle, because a tidy cycle is
 * the one case the free-running counter also got right.
 */
#include <stdio.h>
#include <string.h>

#include "display_pull.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static uint8_t mailbox[4096];

static void set_request(uint32_t idx)
{
    memcpy(mailbox + DISPLAY_PULL_RX_STAT, &idx, sizeof(idx));
}

static uint32_t tx_status(void)
{
    uint32_t v;
    memcpy(&v, mailbox + DISPLAY_PULL_TX_STAT, sizeof(v));
    return v;
}

/* Move writes its own slice into the mailbox before we run; model that so a
 * test can see whether any of Move's bytes survive. */
static void move_writes_its_slice(uint32_t idx)
{
    memcpy(mailbox + DISPLAY_PULL_TX_STAT, &idx, sizeof(idx));
    memset(mailbox + DISPLAY_PULL_TX_DATA, 0xEE, DISPLAY_PULL_SLICE_BYTES);
}

/* A frame whose every byte names the slice it belongs to, so a misplaced
 * slice is visible as the wrong number. */
static void fill_frame(uint8_t *f, uint8_t tag)
{
    for (int i = 0; i < DISPLAY_PULL_FRAME_BYTES; i++)
        f[i] = (uint8_t)(tag + i / DISPLAY_PULL_SLICE_BYTES);
}

static int slice_matches(const uint8_t *frame, uint32_t idx)
{
    int off = (int)(idx - 1) * DISPLAY_PULL_SLICE_BYTES;
    int len = display_pull_slice_len(idx);
    return memcmp(mailbox + DISPLAY_PULL_TX_DATA, frame + off, (size_t)len) == 0;
}

static void test_every_frame_answers_the_request_it_was_given(void)
{
    display_pull_t dp;
    display_pull_reset(&dp);
    uint8_t frame[DISPLAY_PULL_FRAME_BYTES];
    fill_frame(frame, 0x10);

    /* Measured shape: restarts at 1 and 2 in the middle of a panel frame. */
    const uint32_t reqs[] = { 1, 2, 3, 1, 2, 1, 2, 3, 4, 5, 6, 1, 1, 2, 2, 3 };
    for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
        move_writes_its_slice(reqs[i]);
        set_request(reqs[i]);
        uint32_t served = display_pull_serve(&dp, mailbox, frame);
        char msg[96];
        snprintf(msg, sizeof(msg), "frame %zu: request %u answered as %u",
                 i, reqs[i], tx_status());
        CHECK(served == reqs[i] && tx_status() == reqs[i], msg);
        snprintf(msg, sizeof(msg), "frame %zu: slice %u carries another slice's bytes",
                 i, reqs[i]);
        CHECK(slice_matches(frame, reqs[i]), msg);
    }
}

static void test_a_panel_frame_comes_from_one_render(void)
{
    display_pull_t dp;
    display_pull_reset(&dp);
    uint8_t a[DISPLAY_PULL_FRAME_BYTES], b[DISPLAY_PULL_FRAME_BYTES];
    fill_frame(a, 0x10);
    fill_frame(b, 0x80);

    set_request(1);
    display_pull_serve(&dp, mailbox, a);
    /* The UI redraws mid-frame; slices 2..6 must still come from `a`. */
    for (uint32_t idx = 2; idx <= 6; idx++) {
        set_request(idx);
        display_pull_serve(&dp, mailbox, b);
        CHECK(slice_matches(a, idx), "a redraw mid-frame tore the panel frame");
    }
    /* The next request for slice 1 picks the new render up. */
    set_request(1);
    display_pull_serve(&dp, mailbox, b);
    CHECK(slice_matches(b, 1), "slice 1 did not latch the new render");
}

static void test_no_latch_yet_latches_on_any_slice(void)
{
    display_pull_t dp;
    memset(&dp, 0x5A, sizeof(dp));   /* garbage, as after a prior session */
    display_pull_reset(&dp);
    uint8_t frame[DISPLAY_PULL_FRAME_BYTES];
    fill_frame(frame, 0x30);

    set_request(4);
    display_pull_serve(&dp, mailbox, frame);
    CHECK(slice_matches(frame, 4),
          "entering mid-frame sent a stale latch instead of the current render");
}

static void test_out_of_range_request_sends_nothing_of_moves(void)
{
    display_pull_t dp;
    display_pull_reset(&dp);
    uint8_t frame[DISPLAY_PULL_FRAME_BYTES];
    fill_frame(frame, 0x10);

    const uint32_t bad[] = { 0, 7, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        move_writes_its_slice(3);
        set_request(bad[i]);
        CHECK(display_pull_serve(&dp, mailbox, frame) == 0,
              "an out-of-range request was served");
        CHECK(tx_status() == 0, "an out-of-range request left a status behind");
        int clean = 1;
        for (int j = 0; j < DISPLAY_PULL_SLICE_BYTES; j++)
            if (mailbox[DISPLAY_PULL_TX_DATA + j] != 0) clean = 0;
        CHECK(clean, "Move's slice bytes survived an out-of-range request");
    }
}

static void test_last_slice_is_short_and_padded(void)
{
    display_pull_t dp;
    display_pull_reset(&dp);
    uint8_t frame[DISPLAY_PULL_FRAME_BYTES];
    fill_frame(frame, 0x10);

    CHECK(display_pull_slice_len(6) == 164, "slice 6 is not 164 bytes");
    CHECK(display_pull_slice_len(1) == 172, "slice 1 is not 172 bytes");

    move_writes_its_slice(6);
    set_request(6);
    display_pull_serve(&dp, mailbox, frame);
    CHECK(slice_matches(frame, 6), "slice 6 bytes wrong");
    int pad_clean = 1;
    for (int j = 164; j < DISPLAY_PULL_SLICE_BYTES; j++)
        if (mailbox[DISPLAY_PULL_TX_DATA + j] != 0) pad_clean = 0;
    CHECK(pad_clean, "Move's bytes survived past the end of slice 6");
}

int main(void)
{
    test_every_frame_answers_the_request_it_was_given();
    test_a_panel_frame_comes_from_one_render();
    test_no_latch_yet_latches_on_any_slice();
    test_out_of_range_request_sends_nothing_of_moves();
    test_last_slice_is_short_and_padded();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("test_display_pull: all passed\n");
    return 0;
}
