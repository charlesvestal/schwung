/* test_usbc_emit_gate.c - which armed USB-C request may reach the wire.
 *
 * The defect this pins: the retired boot replay and the live monitor-loss
 * repair were armed through ONE variable, so one gate silenced both. Disabling
 * persistence in 1.3.2 therefore also disabled the repair, and a lone 37 12
 * from Move's sampling page left USB-C on the microphone until reboot while
 * Move's own settings page still read Main Out.
 */
#include <stdio.h>
#include "usbc_emit_gate.h"

static int fails;
#define CHECK(c, msg) do { \
    if (!(c)) { fprintf(stderr, "FAIL: %s\n", (msg)); fails++; } \
} while (0)

/* Persistence stays retired: an armed boot replay must never reach the wire. */
static void test_boot_replay_stays_retired(void)
{
    int v = -1;
    CHECK(usbc_emit_select(1, -1, 0, &v) == USBC_EMIT_NONE,
          "retired: a boot replay is dropped while persistence is off");
    CHECK(usbc_emit_select(0, -1, 0, &v) == USBC_EMIT_NONE,
          "retired: a boot replay of Mic is dropped too");
}

/* The whole point of the split: the repair is not persistence and is not
 * gated by it. */
static void test_repair_reaches_the_wire_with_persistence_off(void)
{
    int v = -1;
    CHECK(usbc_emit_select(-1, 1, 0, &v) == USBC_EMIT_REPAIR,
          "repair: reaches the wire with persistence disabled");
    CHECK(v == 1, "repair: carries Main Out");
}

static void test_nothing_armed_emits_nothing(void)
{
    int v = 7;
    CHECK(usbc_emit_select(-1, -1, 0, &v) == USBC_EMIT_NONE, "idle: nothing armed");
    CHECK(usbc_emit_select(-1, -1, 1, &v) == USBC_EMIT_NONE, "idle: nothing armed, persist on");
    CHECK(v == 7, "idle: value untouched");
}

/* The compatibility register still works if persistence is ever revived, so
 * the two paths stay genuinely independent rather than one being dead code. */
static void test_boot_replay_permitted_when_persistence_is_on(void)
{
    int v = -1;
    CHECK(usbc_emit_select(1, -1, 1, &v) == USBC_EMIT_BOOT_REPLAY,
          "revived: a boot replay is permitted when persistence is on");
    CHECK(v == 1, "revived: carries the stored value");
}

/* Both armed: the repair describes what Move is advertising right now, the
 * replay describes a file. Live state wins. */
static void test_repair_wins_over_boot_replay(void)
{
    int v = -1;
    CHECK(usbc_emit_select(0, 1, 1, &v) == USBC_EMIT_REPAIR,
          "both: the live repair takes precedence");
    CHECK(v == 1, "both: repair value is used");
}

int main(void)
{
    test_boot_replay_stays_retired();
    test_repair_reaches_the_wire_with_persistence_off();
    test_nothing_armed_emits_nothing();
    test_boot_replay_permitted_when_persistence_is_on();
    test_repair_wins_over_boot_replay();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
