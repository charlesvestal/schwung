/*
 * xmos_resend.h — re-send Move's XMOS control message when it did not reach
 * the wire.
 *
 * THE PROBLEM. Move writes its 37-family XMOS control messages (37 12 routing
 * + monitoring, 37 14 USB-C out source) into MIDI_OUT and expects them to be
 * transferred. Captured on hardware 2026-09-12: 9 of the 13 it emitted were
 * replaced in the mailbox by an RGB LED SysEx (3b 10) before the transfer, and
 * the loss is silent at every layer — Move's Settings screen shows the value it
 * just sent, the hardware never changed, and the next attempt can be eaten too.
 * Reported as "USB-C Main Out stops working until a reboot".
 *
 * THIS DOES NOT NEED TO KNOW WHO ATE IT. Observe what Move wrote, check the
 * hardware mailbox after the transfer, and if the bytes are not there, put them
 * back. That is deliberate: the culprit is not established (see
 * docs/DIAGNOSTICS.md, "MIDI_OUT loss attribution"), the two candidate windows
 * want opposite fixes, and the user-visible failure is worth closing either
 * way. If the instrument later names a writer, stopping it is the better fix
 * and this becomes a backstop.
 *
 * WHY THIS IS NOT THE PERSISTENCE RETIRED IN 1.3.2. That replayed a value read
 * from a FILE WRITTEN ON A PREVIOUS BOOT, which is how it could assert Main Out
 * — and mute the built-in speaker — against what the user currently wanted.
 * This replays MOVE'S OWN BYTES FROM THIS SESSION, verbatim, milliseconds
 * later. It cannot express an intent the user did not just express, because it
 * has no source of intent other than the message Move itself emitted. It is
 * therefore kept entirely separate from shim_usbc_out_replay and
 * usbc_out_persist_enabled, which stay hard-off.
 *
 * The echo needs no special case: our own re-send landing in the mailbox is
 * what CONFIRMS the watch, because confirmation asks "are these bytes on the
 * wire", not "who put them there".
 *
 * Pure: no allocation, no I/O, no locks, no globals. Safe on the SPI callback,
 * and runnable on the host — tests/host/test_xmos_resend.c replays the captured
 * failure.
 */
#ifndef XMOS_RESEND_H
#define XMOS_RESEND_H

#include <stdint.h>
#include <string.h>

#include "shadow_xmos_audio.h"   /* XMOS_AUDIO_MSG_LEN, the 23-byte envelope */

/* Move sends at most the pair. A third in one frame would be a protocol we have
 * never seen; it is dropped rather than growing the struct. */
#define XMOS_RESEND_MAX_MSGS 2

/* Per message. Three frames of trying is ~9 ms at the SPI rate, far longer than
 * the contention that causes the loss, and bounded so a genuinely wedged
 * mailbox cannot make us emit forever. */
#define XMOS_RESEND_MAX_ATTEMPTS 3

typedef struct {
    uint8_t msg[XMOS_AUDIO_MSG_LEN];
    uint8_t used;
    uint8_t attempts;      /* re-sends already queued for this message */
    uint8_t queued;        /* a re-send is waiting to be emitted */
    /* Budget spent and the bytes still never arrived. The slot is KEPT so
     * observe() recognises them and does not start over — clearing it instead
     * let the next frame re-watch the same message with a fresh budget, which
     * is an unbounded re-send loop. Found by test_budget_is_bounded: 150 sends
     * across 200 frames where 3 were intended. Cleared when a DIFFERENT
     * message arrives, because that is new intent and this one is moot. */
    uint8_t abandoned;
} xmos_resend_slot_t;

typedef struct {
    xmos_resend_slot_t watch[XMOS_RESEND_MAX_MSGS];
    /* Diagnostics, read by the worker (which may log) and never by the
     * callback. Monotonic. */
    uint32_t lost;         /* a watched message was absent from the mailbox */
    uint32_t resent;       /* bytes handed back to the emitter */
    uint32_t gave_up;      /* attempts exhausted; the loss stands */
    uint32_t delivered;    /* confirmed on the wire (first try or after) */
} xmos_resend_t;

#define XMOS_RESEND_INIT { {{{0}, 0, 0, 0, 0}}, 0, 0, 0, 0 }

/* ---- envelope scanning ---------------------------------------------------
 * Walk a MIDI_OUT region (4-byte USB-MIDI slots) and reassemble cable-0 SysEx.
 * Only complete 37-family Ableton envelopes of exactly XMOS_AUDIO_MSG_LEN are
 * reported; anything else is skipped without disturbing the walk. Slot order is
 * wire order, so this is also the order they would have gone out in.
 */
static inline int xmos_resend_scan(const uint8_t *midi_out, int len,
                                  uint8_t out[][XMOS_AUDIO_MSG_LEN], int max)
{
    if (!midi_out || !out || max <= 0) return 0;

    uint8_t buf[XMOS_AUDIO_MSG_LEN + 8];
    int blen = 0;
    int active = 0;
    int found = 0;

    for (int i = 0; i + 4 <= len; i += 4) {
        uint8_t cin = midi_out[i] & 0x0F;
        uint8_t cable = (uint8_t)((midi_out[i] >> 4) & 0x0F);
        if (cable != 0) continue;

        int payload;
        int ends;
        switch (cin) {
        case 0x04: payload = 3; ends = 0; break;
        case 0x05: payload = 1; ends = 1; break;
        case 0x06: payload = 2; ends = 1; break;
        case 0x07: payload = 3; ends = 1; break;
        default: continue;
        }

        if (midi_out[i + 1] == 0xF0) { blen = 0; active = 1; }
        if (!active) continue;

        for (int p = 0; p < payload; p++) {
            if (blen < (int)sizeof(buf)) buf[blen++] = midi_out[i + 1 + p];
        }

        if (!ends) continue;
        active = 0;

        if (blen != XMOS_AUDIO_MSG_LEN) continue;
        if (buf[0] != 0xF0 || buf[1] != 0x00 || buf[2] != 0x21 || buf[3] != 0x1D)
            continue;
        if (buf[4] != 0x01 || buf[5] != 0x01 || buf[6] != 0x37) continue;

        if (found < max) memcpy(out[found], buf, XMOS_AUDIO_MSG_LEN);
        found++;
    }
    return found < max ? found : max;
}

static inline int xmos_resend_same(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, XMOS_AUDIO_MSG_LEN) == 0;
}

static inline int xmos_resend_present(const uint8_t *midi_out, int len,
                                     const uint8_t *msg)
{
    uint8_t seen[XMOS_RESEND_MAX_MSGS + 2][XMOS_AUDIO_MSG_LEN];
    int n = xmos_resend_scan(midi_out, len, seen, XMOS_RESEND_MAX_MSGS + 2);
    for (int i = 0; i < n; i++)
        if (xmos_resend_same(seen[i], msg)) return 1;
    return 0;
}

/* ---- the state machine --------------------------------------------------- */

/* Called in pre_transfer, EARLY — before Schwung's own MIDI_OUT writers run, so
 * a message lost to our own work is still watched. Observing at the end of
 * pre_transfer instead would only ever see the survivors, which is the half of
 * the problem that needs no defence.
 *
 * Re-observing bytes already watched does NOT reset the attempt budget: our own
 * re-send passes through here too, and a fresh budget each time is an
 * unbounded loop. A DIFFERENT message supersedes the slot — a new selection is
 * new intent and the old one no longer matters. */
static inline void xmos_resend_observe(xmos_resend_t *st, const uint8_t *midi_out,
                                       int len)
{
    if (!st || !midi_out) return;

    uint8_t seen[XMOS_RESEND_MAX_MSGS][XMOS_AUDIO_MSG_LEN];
    int n = xmos_resend_scan(midi_out, len, seen, XMOS_RESEND_MAX_MSGS);
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        int slot = -1;

        /* Already watching these exact bytes? Keep the budget. */
        for (int s = 0; s < XMOS_RESEND_MAX_MSGS; s++) {
            if (st->watch[s].used && xmos_resend_same(st->watch[s].msg, seen[i])) {
                slot = s;
                break;
            }
        }
        if (slot >= 0) continue;

        /* These bytes are new intent, so every ABANDONED slot is stale: it
         * describes a selection the user has since moved on from. Retire them
         * first — that both frees a slot and stops a later repeat of an
         * abandoned message being ignored forever. */
        for (int a = 0; a < XMOS_RESEND_MAX_MSGS; a++) {
            if (st->watch[a].abandoned) {
                st->watch[a].used = 0;
                st->watch[a].abandoned = 0;
            }
        }

        /* A free slot, else the one with the most attempts spent — it is the
         * closest to being abandoned anyway. Never stays -1. */
        for (int s = 0; s < XMOS_RESEND_MAX_MSGS; s++) {
            if (!st->watch[s].used) { slot = s; break; }
        }
        if (slot < 0) {
            slot = 0;
            for (int s = 1; s < XMOS_RESEND_MAX_MSGS; s++)
                if (st->watch[s].attempts > st->watch[slot].attempts) slot = s;
        }

        memcpy(st->watch[slot].msg, seen[i], XMOS_AUDIO_MSG_LEN);
        st->watch[slot].used = 1;
        st->watch[slot].attempts = 0;
        st->watch[slot].queued = 0;
        st->watch[slot].abandoned = 0;
    }
}

/* Called in post_transfer against the HARDWARE mailbox — the bytes that
 * actually went out. A watched message found there is delivered, whoever put it
 * there; absent, it is queued for another go until the budget runs out. */
static inline void xmos_resend_confirm(xmos_resend_t *st, const uint8_t *hw_midi_out,
                                       int len)
{
    if (!st || !hw_midi_out) return;

    for (int s = 0; s < XMOS_RESEND_MAX_MSGS; s++) {
        xmos_resend_slot_t *w = &st->watch[s];
        if (!w->used || w->queued || w->abandoned) continue;

        if (xmos_resend_present(hw_midi_out, len, w->msg)) {
            st->delivered++;
            w->used = 0;
            w->attempts = 0;
            continue;
        }

        st->lost++;
        if (w->attempts >= XMOS_RESEND_MAX_ATTEMPTS) {
            st->gave_up++;
            w->queued = 0;
            w->abandoned = 1;   /* kept, so observe() will not start over */
            continue;
        }
        w->attempts++;
        w->queued = 1;
    }
}

/* Called in pre_transfer by the emitter. Copies one queued message out and
 * clears the queue flag; the slot stays WATCHED, so the re-send is itself
 * checked next post_transfer. Returns 1 when a message was handed over. */
static inline int xmos_resend_take(xmos_resend_t *st, uint8_t out[XMOS_AUDIO_MSG_LEN])
{
    if (!st || !out) return 0;
    for (int s = 0; s < XMOS_RESEND_MAX_MSGS; s++) {
        xmos_resend_slot_t *w = &st->watch[s];
        if (!w->used || !w->queued || w->abandoned) continue;
        memcpy(out, w->msg, XMOS_AUDIO_MSG_LEN);
        w->queued = 0;
        st->resent++;
        return 1;
    }
    return 0;
}

static inline int xmos_resend_has_work(const xmos_resend_t *st)
{
    if (!st) return 0;
    for (int s = 0; s < XMOS_RESEND_MAX_MSGS; s++)
        if (st->watch[s].used && st->watch[s].queued && !st->watch[s].abandoned)
            return 1;
    return 0;
}

#endif /* XMOS_RESEND_H */
