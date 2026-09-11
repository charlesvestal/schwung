/* usbc_emit_gate.h - which armed USB-C request may reach the XMOS wire.
 *
 * TWO PRODUCERS, AND THEY ARE NOT THE SAME FEATURE
 *
 *   boot replay  Retired in 1.3.2. It restored a preference from a FILE that
 *                Move itself was not advertising, so a device booted with its
 *                built-in speaker muted (monitoring is how Main Out reaches
 *                USB-C, and the XMOS mutes the speaker while it is set) and
 *                Move's own settings page reading Mic. Nothing on screen
 *                explained it, which is why it had to go.
 *
 *   repair       The lone-37-12 defence. It fires only while Move's LIVE
 *                37 14 still says Main Out — it restores the mode Move is
 *                advertising this second and invents nothing. Without it,
 *                Move's sampling page clears monitoring behind our back
 *                (it emits a lone 37 12 carrying bit1 from its own stale
 *                "Mic" UI state) and USB-C silently carries the microphone
 *                until the next reboot, with Move's settings page insisting
 *                otherwise and re-picking Main Out emitting nothing.
 *
 * They were armed through ONE variable and therefore shared one gate, so
 * retiring the first silenced the second: 1.3.2 fixed the muted speaker and
 * shipped the stuck microphone. Selection is kept pure so tests/host can
 * drive it — the wiring, not the arithmetic, is where this went wrong.
 *
 * `boot_replay` and `repair` are armed values (0 = Mic, 1 = Main Out) or -1
 * for "not armed", read straight off the worker's request variables.
 */
#ifndef USBC_EMIT_GATE_H
#define USBC_EMIT_GATE_H

typedef enum {
    USBC_EMIT_NONE        = 0,
    USBC_EMIT_BOOT_REPLAY = 1,
    USBC_EMIT_REPAIR      = 2,
} usbc_emit_kind_t;

/* Returns what to put on the wire and writes its value to *value. *value is
 * left untouched when nothing is emitted. */
static inline usbc_emit_kind_t usbc_emit_select(int boot_replay, int repair,
                                                int persist_enabled, int *value)
{
    /* The repair describes what Move is advertising right now; the replay
     * describes a file. When both are armed, live state wins. */
    if (repair >= 0) {
        if (value) *value = repair;
        return USBC_EMIT_REPAIR;
    }
    if (boot_replay >= 0 && persist_enabled) {
        if (value) *value = boot_replay;
        return USBC_EMIT_BOOT_REPLAY;
    }
    return USBC_EMIT_NONE;
}

#endif /* USBC_EMIT_GATE_H */
