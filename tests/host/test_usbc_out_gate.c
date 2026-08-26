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

/* ---- monitor-loss defence -------------------------------------------- */

static void mon(usbc_gate_t *g, int usbc_out, int monitor)
{
    clr();
    usbc_gate_tick_monitor(g, usbc_out, monitor, &OUT);
}

/* Drive a gate to SETTLED with Main Out stored and monitoring live, the way
 * the wire does. */
static void settle_main_out(usbc_gate_t *g)
{
    usbc_gate_init(g, 1);
    observe(g, 0);        /* Move's boot default */
    boot_replay(g);
    observe(g, 1);        /* our re-assert lands -> settled */
    mon(g, 1, 1);         /* monitoring live */
}

/* The hardware case: sampling-source change emits a lone 37 12 with bit1
 * clear, so monitor drops while 37 14 still says Main Out. */
static void test_monitor_loss_triggers_reassert(void)
{
    usbc_gate_t g;
    settle_main_out(&g);

    mon(&g, 1, 0);
    CHECK(OUT.replay == 0, "monloss: does not fire on the first tick (debounce)");

    mon(&g, 1, 0);
    CHECK(OUT.replay == 1 && OUT.replay_value == 1,
          "monloss: re-asserts Main Out once the condition holds");
    CHECK(OUT.persist == 0, "monloss: re-asserting persists nothing");
}

/* A deliberate Mic selection moves 37 14 too. If the pair splits across SPI
 * frames we may see monitor=0 one tick before usbc_out=0 — that must never
 * become a re-assert, or we would fight the user. */
static void test_split_mic_selection_is_not_fought(void)
{
    usbc_gate_t g;
    settle_main_out(&g);

    mon(&g, 1, 0);        /* first frame of the split pair */
    CHECK(OUT.replay == 0, "split: no action on the first tick");

    observe(&g, 0);       /* 37 14 00 arrives -> user really chose Mic */
    mon(&g, 0, 0);
    CHECK(OUT.replay == 0, "split: a real Mic selection is never fought");

    mon(&g, 0, 0);
    CHECK(OUT.replay == 0, "split: still not fought on later ticks");
}

static void test_monitor_healthy_does_nothing(void)
{
    usbc_gate_t g;
    settle_main_out(&g);
    for (int i = 0; i < 5; i++) {
        mon(&g, 1, 1);
        CHECK(OUT.replay == 0 && OUT.persist == 0, "healthy: monitoring live, no action");
    }
}

static void test_monitor_loss_not_defended_when_mic_stored(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 0);
    boot_replay(&g);      /* stored Mic settles immediately */
    for (int i = 0; i < 4; i++) {
        mon(&g, 1, 0);
        CHECK(OUT.replay == 0, "mic-stored: nothing to defend");
    }
}

/* Boot arbitration owns the wire; the monitor defence must not cut in. */
static void test_monitor_defence_inert_before_settling(void)
{
    usbc_gate_t g;
    usbc_gate_init(&g, 1);
    for (int i = 0; i < 4; i++) {
        mon(&g, 1, 0);
        CHECK(OUT.replay == 0, "pre-settle: monitor defence is inert");
    }
    boot_replay(&g);
    for (int i = 0; i < 4; i++) {
        mon(&g, 1, 0);
        CHECK(OUT.replay == 0, "defending: monitor defence is inert");
    }
}

static void test_monitor_reasserts_are_bounded(void)
{
    usbc_gate_t g;
    settle_main_out(&g);

    int replays = 0;
    for (int i = 0; i < (USBC_GATE_MAX_REPLAYS + 5) * USBC_GATE_MONITOR_DEBOUNCE; i++) {
        mon(&g, 1, 0);
        if (OUT.replay) replays++;
    }
    CHECK(replays == USBC_GATE_MAX_REPLAYS, "monbound: re-asserts stop at the cap");
}

/* A fourth sampling-source change must still be defended, so the budget
 * re-arms on each fresh 1->0 transition rather than draining for the session. */
static void test_monitor_budget_rearms_on_fresh_loss(void)
{
    usbc_gate_t g;
    settle_main_out(&g);

    for (int round = 0; round < 4; round++) {
        mon(&g, 1, 1);                     /* our re-assert took effect */
        mon(&g, 1, 0);                     /* fresh loss: debounce tick */
        mon(&g, 1, 0);
        CHECK(OUT.replay == 1, "rearm: every fresh monitor loss is defended");
    }
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
    test_monitor_loss_triggers_reassert();
    test_split_mic_selection_is_not_fought();
    test_monitor_healthy_does_nothing();
    test_monitor_loss_not_defended_when_mic_stored();
    test_monitor_defence_inert_before_settling();
    test_monitor_reasserts_are_bounded();
    test_monitor_budget_rearms_on_fresh_loss();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
