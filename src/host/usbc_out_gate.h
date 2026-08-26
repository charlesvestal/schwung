/* usbc_out_gate.h - Boot arbitration for the USB-C audio-out preference.
 *
 * THE PROBLEM THIS SOLVES
 *
 * Move's firmware forgets the USB-C audio-out source across reboots and
 * asserts its own Mic default on every boot. Schwung remembers the user's
 * preference and re-asserts it a few seconds in. Both arrive on the wire as
 * the same `37 14` envelope, so nothing in the bytes says which is which.
 *
 * The original gate separated them by wall clock: ignore everything for the
 * first ~7 s, trust everything after. That races a variable-latency firmware
 * event with a fixed deadline. The worker's clock starts when MoveOriginal
 * opens the SPI device; Move's default assert floats with boot load. When a
 * slow boot pushed the assert past the deadline, the observed Mic was taken
 * for a user choice and written to the state file — so the preference both
 * reverted in that session AND was forgotten on the next boot. One mechanism,
 * both halves of the reported symptom, intermittent by construction.
 *
 * THE SIGNAL THAT ACTUALLY SEPARATES THEM
 *
 * We only ever re-assert Main Out (1) — replaying Mic would be pointless,
 * since Mic is Move's own boot default. So during the boot window an observed
 * **0 can only have come from Move**, never from us. That, not elapsed time,
 * is the discriminator.
 *
 * The gate therefore stays closed until Move has had its say AND we have
 * re-asserted over it:
 *
 *   phase 0  pre-replay   observations recorded, never persisted
 *   phase 1  defending    an observed 0 is Move's default -> re-assert, never
 *                         persist; an observed 1 settles us, but only once
 *                         Move has actually asserted 0 this boot
 *   phase 2  settled      ordinary operation: persist anything that differs
 *
 * A slow boot now lands in phase 1 with `saw_mic_assert` still clear, so our
 * own replay echo cannot settle the gate prematurely; Move's late assert is
 * countered rather than believed.
 *
 * Everything here is pure state — no I/O, no allocation, no locks, no clock —
 * so the worker can call it and the host suite can compile it directly.
 * Timekeeping stays with the caller, which is what makes this testable.
 */
#ifndef USBC_OUT_GATE_H
#define USBC_OUT_GATE_H

#include <stdint.h>

/* Bounded so a Move that re-asserts its default repeatedly cannot start a
 * replay war with us. Three attempts is well past any observed boot. */
#define USBC_GATE_MAX_REPLAYS 3

typedef enum {
    USBC_GATE_PHASE_PRE_REPLAY = 0,
    USBC_GATE_PHASE_DEFENDING  = 1,
    USBC_GATE_PHASE_SETTLED    = 2,
} usbc_gate_phase_t;

/* Ticks the monitor-loss condition must hold before we act. One tick of
 * hysteresis, i.e. ~400 ms at the worker's 200 ms cadence. See
 * usbc_gate_tick_monitor. */
#define USBC_GATE_MONITOR_DEBOUNCE 2

typedef struct {
    int8_t  stored;          /* the persisted preference: -1 unknown, 0 Mic, 1 Main Out */
    uint8_t phase;           /* usbc_gate_phase_t */
    uint8_t replays_left;
    uint8_t saw_mic_assert;  /* Move has asserted Mic at least once this boot */

    /* Monitor-loss defence, independent of the boot budget above. */
    int8_t  last_monitor;
    uint8_t monitor_pending;
    uint8_t monitor_replays_left;
} usbc_gate_t;

/* What the caller must do as a result of a gate call. Both may be set. */
typedef struct {
    int    persist;          /* 1 = write persist_value to the state file */
    int8_t persist_value;
    int    replay;           /* 1 = put replay_value on the wire */
    int8_t replay_value;
} usbc_gate_out_t;

/* `stored` is the persisted preference, or -1 if the file is absent/unreadable. */
void usbc_gate_init(usbc_gate_t *g, int stored);

/* Call once, when the caller judges Move's firmware ready to accept SysEx
 * (the existing ~5 s mark). Arms the boot re-assert when the stored
 * preference is Main Out; otherwise settles immediately, because a stored Mic
 * needs no defending — it agrees with Move's own default, and the ordinary
 * differs-from-stored test is enough from that point on. */
void usbc_gate_boot_replay(usbc_gate_t *g, usbc_gate_out_t *out);

/* Call whenever the wire reports a value (0 = Mic, 1 = Main Out). */
void usbc_gate_observe(usbc_gate_t *g, int observed, usbc_gate_out_t *out);

/* Call once per worker tick with the last values seen on the wire (-1 =
 * unknown): `usbc_out` from 37 14, `monitor` from bit1 of 37 12.
 *
 * Defends against a second, separate way the preference is lost. Move's
 * sampling page emits a **lone 37 12** to set bit0 (the USB-C input select)
 * and carries bit1 from its own permanently-stale "Mic" UI state. Observed on
 * hardware 2026-08-26: `37 12 01` then `37 12 00`, with no 37 14 anywhere near
 * either. Monitoring is *how* Main Out reaches USB-C, so clearing it reverts
 * the hardware — while 37 14 still says Main Out, so nothing re-asserts and
 * the revert is silent. Changing the sampling source therefore killed USB-C
 * out, which is the in-session half of the original bug report.
 *
 * `usbc_out == 1 && monitor == 0` is what separates this from a deliberate Mic
 * selection, where 37 14 moves too. The pair CAN split across SPI frames
 * (16 of 20 MIDI_OUT slots — concurrent LED traffic forces a split), so acting
 * on the first frame of a split Mic selection would fight the user. Hence the
 * USBC_GATE_MONITOR_DEBOUNCE hysteresis: frames are ~3 ms and the worker ticks
 * every 200 ms, so a split resolves long before two consecutive ticks agree.
 *
 * Bounded like the boot replay, and re-armed on each fresh 1->0 transition so
 * a fourth sampling-source change is still defended. */
void usbc_gate_tick_monitor(usbc_gate_t *g, int usbc_out, int monitor,
                            usbc_gate_out_t *out);

/* Backstop: force the gate open. The caller applies this on a generous
 * deadline so that a boot where Move never speaks at all cannot leave the
 * gate closed forever, silently swallowing every later user change. Nothing
 * has been persisted wrongly by that point — settling only permits future
 * writes, it does not perform one. */
void usbc_gate_force_settle(usbc_gate_t *g);

#endif /* USBC_OUT_GATE_H */
