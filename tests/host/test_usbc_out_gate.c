/* Unit test for the USB-C audio-out boot gate.
 *
 * The bug this pins: a stored Main Out preference was being overwritten with
 * Mic whenever Move's own boot-default assert landed after the old fixed ~7 s
 * deadline. The device evidence was a state file that had gone 1 -> 0 between
 * boots with no user action — one boot logging "boot re-assert Main Out", the
 * next not logging it at all.
 *
 * Build/run: bash tests/host/test_usbc_out_gate.sh
 */
#include <stdio.h>
#include <string.h>
#include "usbc_out_gate.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

/* Every call clears its output first, so a test that expects "no action" is
 * checking the gate stayed silent rather than inheriting a stale struct. */
static usbc_gate_out_t OUT;
static void clr(void) { memset(&OUT, 0, sizeof OUT); }

static void observe(usbc_gate_t *g, int v) { clr(); usbc_gate_observe(g, v, &OUT); }
static void boot_replay(usbc_gate_t *g)    { clr(); usbc_gate_boot_replay(g, &OUT); }

/* ---- the regression itself ------------------------------------------- */

/* Move's default assert lands AFTER our re-assert. Under the old wall-clock
 * gate this was read as a user choice and written to the file. */
static void test_late_move_assert_does_not_clobber(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);            /* stored: Main Out */

    boot_replay(&g);
    CHECK(OUT.replay == 1 && OUT.replay_value == 1, "late: boot arms a Main Out replay");
    CHECK(OUT.persist == 0, "late: arming a replay persists nothing");

    observe(&g, 1);                   /* our own replay echoing back */
    CHECK(OUT.persist == 0, "late: our own echo is never persisted");

    observe(&g, 0);                   /* Move's default, arriving late */
    CHECK(OUT.persist == 0, "late: Move's late default must NOT be persisted");
    CHECK(OUT.replay == 1 && OUT.replay_value == 1,
          "late: Move's late default is countered by another re-assert");

    observe(&g, 1);                   /* the counter-assert lands */
    CHECK(OUT.persist == 0, "late: counter-assert echo is not persisted");

    /* Gate is now open; a genuine user choice must get through. */
    observe(&g, 0);
    CHECK(OUT.persist == 1 && OUT.persist_value == 0,
          "late: a user Mic choice after settling IS persisted");
}

/* The fast boot that used to work: Move asserts before we replay. */
static void test_early_move_assert(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);

    observe(&g, 0);                   /* Move's default, pre-replay */
    CHECK(OUT.persist == 0, "early: pre-replay observations are never persisted");
    CHECK(OUT.replay == 0, "early: nothing is put on the wire before boot_replay");

    boot_replay(&g);
    CHECK(OUT.replay == 1 && OUT.replay_value == 1, "early: replay armed");

    observe(&g, 1);                   /* echo; Move has already spoken */
    CHECK(OUT.persist == 0, "early: echo not persisted");

    observe(&g, 0);
    CHECK(OUT.persist == 1 && OUT.persist_value == 0,
          "early: user Mic choice persists once settled");
}

/* ---- stored Mic / unknown -------------------------------------------- */

static void test_stored_mic_settles_immediately(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 0);

    boot_replay(&g);
    CHECK(OUT.replay == 0, "mic: nothing to re-assert when Mic is stored");

    observe(&g, 0);
    CHECK(OUT.persist == 0, "mic: Move's default matches stored, no write");

    observe(&g, 1);
    CHECK(OUT.persist == 1 && OUT.persist_value == 1, "mic: user Main Out persists");

    observe(&g, 1);
    CHECK(OUT.persist == 0, "mic: repeat of the same value writes nothing");
}

static void test_unknown_stored_records_first_value(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, -1);           /* no state file yet */

    boot_replay(&g);
    CHECK(OUT.replay == 0, "unknown: no replay without a stored preference");

    observe(&g, 0);
    CHECK(OUT.persist == 1 && OUT.persist_value == 0, "unknown: first value is recorded");
}

/* ---- bounds and backstops -------------------------------------------- */

/* A Move that keeps re-asserting must not start an unbounded replay war. */
static void test_replays_are_bounded(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);

    /* The cap counts every re-assert this boot, the one boot_replay puts on
     * the wire included — not just the ones Move provokes afterwards. */
    boot_replay(&g);
    int replays = OUT.replay ? 1 : 0;

    for (int i = 0; i < USBC_GATE_MAX_REPLAYS + 5; i++) {
        observe(&g, 0);
        CHECK(OUT.persist == 0, "bounded: a defended Mic is never persisted");
        if (OUT.replay) replays++;
    }
    CHECK(replays == USBC_GATE_MAX_REPLAYS, "bounded: re-asserts stop at the cap");

    /* Having given up, the gate must be open — otherwise user changes would be
     * swallowed for the rest of the session. */
    observe(&g, 1);
    CHECK(OUT.persist == 1 && OUT.persist_value == 1,
          "bounded: gate opens after giving up so user changes still persist");
}

/* If Move never speaks at all this boot, the gate must not stay shut forever. */
static void test_force_settle_backstop(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);
    boot_replay(&g);

    observe(&g, 1);
    CHECK(OUT.persist == 0, "backstop: still defending before the deadline");

    usbc_gate_force_settle(&g);

    observe(&g, 0);
    CHECK(OUT.persist == 1 && OUT.persist_value == 0,
          "backstop: forcing the gate open lets user changes through");
}

/* force_settle only permits future writes; it must never itself persist. */
static void test_force_settle_writes_nothing(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);
    boot_replay(&g);
    observe(&g, 0);                   /* defended */
    usbc_gate_force_settle(&g);
    observe(&g, 1);
    CHECK(OUT.persist == 0,
          "backstop: settling does not retroactively persist the defended value");
}

/* The stored value must track what we write, or a value would be written twice
 * and — worse — a return to the original would look like "no change". */
static void test_stored_tracks_writes(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 0);
    boot_replay(&g);

    observe(&g, 1);
    CHECK(OUT.persist == 1 && OUT.persist_value == 1, "tracks: first change written");
    observe(&g, 0);
    CHECK(OUT.persist == 1 && OUT.persist_value == 0, "tracks: change back is written");
    observe(&g, 0);
    CHECK(OUT.persist == 0, "tracks: no redundant write");
}

int main(void)
{
    test_late_move_assert_does_not_clobber();
    test_early_move_assert();
    test_stored_mic_settles_immediately();
    test_unknown_stored_records_first_value();
    test_replays_are_bounded();
    test_force_settle_backstop();
    test_force_settle_writes_nothing();
    test_stored_tracks_writes();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
