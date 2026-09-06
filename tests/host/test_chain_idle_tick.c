/*
 * The idle-tick handshake between the shim's idle gate and the chain host.
 *
 * The interesting failure is not a compile error, it is a LEAK: if the
 * `advanced` guard is ever left standing after a frame that does not render,
 * the next render silently skips one LFO and MIDI FX tick — forever, with no
 * symptom anyone can attribute. The original change was pinned by a shell
 * script grepping for nine exact source lines, which cannot see that at all.
 * The transitions live in a header so this can drive them.
 */
#include <assert.h>
#include <stdio.h>
#include "chain_idle_tick.h"

/* One idle, non-probe frame: the shim ticks, then asks once whether to wake. */
static int idle_frame(chain_idle_tick_t *st, int delivered) {
    chain_idle_tick_mark(st, delivered);
    return chain_idle_tick_take(st);
}

int main(void) {
    chain_idle_tick_t st = {0, 0};

    /* ---- a rendering frame owes its own tick ---------------------------- */
    assert(chain_idle_tick_consume(&st) == 1);
    assert(chain_idle_tick_consume(&st) == 1);   /* and stays that way */

    /* ---- idle frame, nothing delivered: no wake, no residue -------------
     * There will be no render to consume the guard, so take() must clear it.
     * Left standing, the NEXT render (a probe frame, or a note-on) would skip
     * its tick — the doubled-arp bug's mirror image, and much harder to see. */
    assert(idle_frame(&st, 0) == 0);
    assert(st.advanced == 0);
    assert(st.wake == 0);
    assert(chain_idle_tick_consume(&st) == 1);   /* probe frame still ticks */

    /* ---- idle frame, delivered: wake, and the render must NOT re-tick ---- */
    assert(idle_frame(&st, 1) == 1);
    assert(st.advanced == 1);                    /* survives take() */
    assert(chain_idle_tick_consume(&st) == 0);   /* mod:tick already did it */
    assert(st.advanced == 0);
    assert(chain_idle_tick_consume(&st) == 1);   /* one block only */

    /* ---- take() is ONE-SHOT ---------------------------------------------
     * The shim asks once per frame. A second ask must not wake a second
     * block, or a single generated note holds the slot out of idle. */
    chain_idle_tick_mark(&st, 1);
    assert(chain_idle_tick_take(&st) == 1);
    assert(chain_idle_tick_take(&st) == 0);
    /* …and the repeat ask, seeing no wake, clears the guard the first ask
     * left for the render. That is a real ordering constraint on the shim:
     * take() exactly once, before render_block. */
    assert(st.advanced == 0);

    /* ---- a shim too old to dlsym the export -----------------------------
     * chain_take_midi_tick_wake is NULL-checked at the call site, so mark()
     * can run with no take() at all. The cost must be bounded at one skipped
     * tick on the next render, never a latched guard. */
    st.advanced = 0; st.wake = 0;
    chain_idle_tick_mark(&st, 1);
    chain_idle_tick_mark(&st, 1);
    chain_idle_tick_mark(&st, 0);
    assert(chain_idle_tick_consume(&st) == 0);   /* the one skipped tick */
    assert(chain_idle_tick_consume(&st) == 1);   /* recovered */

    /* ---- mark() overwrites, never accumulates ---------------------------
     * A delivered frame followed by an empty one must not stay woken. */
    chain_idle_tick_mark(&st, 1);
    chain_idle_tick_mark(&st, 0);
    assert(chain_idle_tick_take(&st) == 0);

    /* ---- NULL instance ---------------------------------------------------
     * consume() answers 1: a caller with no state owes its tick, because
     * skipping is the outcome that freezes an LFO. */
    assert(chain_idle_tick_consume(NULL) == 1);
    assert(chain_idle_tick_take(NULL) == 0);
    chain_idle_tick_mark(NULL, 1);

    printf("PASS: chain idle-tick handshake\n");
    return 0;
}
