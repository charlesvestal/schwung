/*
 * mute_follow.h — keep a shadow slot's mute equal to Move's track mute.
 *
 * Mute (CC 88) passes through to Move, so Mute+Track mutes BOTH the Move track
 * and the shadow slot. The slot used to TOGGLE its own bit, which is only in
 * sync while the two already agree: mute the Move track with a plain Mute tap
 * (Move's selected track only) and every later Mute+Track flips the two in
 * opposite directions, forever.
 *
 * So Move is the authority, and Move says what it did: "<track name> muted" /
 * "<track name> unmuted" over com.ableton.move.ScreenReader.text, which
 * shadow_dbus.c already receives. The announcement names the INSTRUMENT, not
 * the track, so it cannot say which track by itself — and that is exactly the
 * hole the removed D-Bus auto-correct fell into: it applied every " muted"
 * suffix to the selected slot, and Move utters drum-cell mutes the same way
 * ("Tom 808 Low 2 muted"), so kit names muted slots across every project.
 *
 * Here the TRACK comes from the gesture and only the STATE from the text:
 *
 *   Mute pressed          -> target = Move's selected track (plain Mute tap)
 *   Track N while held    -> target = N (Mute+Track)
 *   anything else held    -> target = none (Mute+pad is a drum-CELL mute)
 *   Mute released         -> the target stays open MUTE_FOLLOW_WINDOW_MS
 *
 * An announcement outside that window, or with no target, changes nothing.
 * Schwung never utters " muted" itself, and a pad announcement cannot arrive
 * with a target because the pad press cleared it.
 *
 * The plain-tap target needs Move's selected track, which Schwung only knows
 * from Track presses it has watched; before the first one it is a default, so
 * a plain tap then attributes nothing (`selection_known`).
 *
 * Written on the SPI callback, read on the D-Bus thread. Each field is a
 * single aligned word; a torn pair can at worst attribute one announcement to
 * a window that just closed, which applies Move's own state to the slot the
 * user just pressed.
 *
 * Pure: no allocation, no I/O, no clock — the caller passes `now_ms`.
 */
#ifndef MUTE_FOLLOW_H
#define MUTE_FOLLOW_H

#include <stdint.h>
#include <stddef.h>

#define MUTE_FOLLOW_WINDOW_MS 1000u

typedef enum {
    MUTE_ANNOUNCE_NONE    = 0,
    MUTE_ANNOUNCE_MUTED   = 1,
    MUTE_ANNOUNCE_UNMUTED = 2,
} mute_announce_t;

typedef struct {
    volatile int      held;           /* Mute is down */
    volatile int      target;         /* slot the next announcement belongs to, -1 none */
    volatile uint64_t deadline_ms;    /* after release: window end; 0 while held */
} mute_follow_t;

static inline void mute_follow_reset(mute_follow_t *f)
{
    f->held = 0;
    f->target = -1;
    f->deadline_ms = 0;
}

static inline void mute_follow_on_mute_press(mute_follow_t *f, int selected_slot,
                                             int selection_known, int n_slots)
{
    f->held = 1;
    f->deadline_ms = 0;
    f->target = (selection_known && selected_slot >= 0 && selected_slot < n_slots)
                ? selected_slot : -1;
}

static inline void mute_follow_on_track_press(mute_follow_t *f, int slot, int n_slots)
{
    if (!f->held) return;
    f->target = (slot >= 0 && slot < n_slots) ? slot : -1;
}

/* Any other press while Mute is down (a pad, a step, another button) makes
 * the gesture something other than a track mute. */
static inline void mute_follow_on_other_press(mute_follow_t *f)
{
    if (!f->held) return;
    f->target = -1;
}

static inline void mute_follow_on_mute_release(mute_follow_t *f, uint64_t now_ms)
{
    if (!f->held) return;
    f->held = 0;
    f->deadline_ms = now_ms + MUTE_FOLLOW_WINDOW_MS;
}

/* Which slot an announcement arriving now belongs to, or -1. */
static inline int mute_follow_target(const mute_follow_t *f, uint64_t now_ms)
{
    int t = f->target;
    if (t < 0) return -1;
    if (f->held) return t;
    if (f->deadline_ms == 0 || now_ms > f->deadline_ms) return -1;
    return t;
}

static inline int mute_follow_ends_with(const char *s, size_t n, const char *suf)
{
    size_t m = 0;
    while (suf[m]) m++;
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++) {
        char c = s[n - m + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return 0;
    }
    return 1;
}

/*
 * " unmuted" is tested first: it also ends in "muted". Requires a name before
 * the suffix — a bare "muted" names nothing. Trailing whitespace is ignored.
 */
static inline mute_announce_t mute_announce_classify(const char *text)
{
    if (!text) return MUTE_ANNOUNCE_NONE;
    size_t n = 0;
    while (text[n]) n++;
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n' ||
                     text[n - 1] == '\r' || text[n - 1] == '\t')) n--;
    if (mute_follow_ends_with(text, n, " unmuted") && n > 8) return MUTE_ANNOUNCE_UNMUTED;
    if (mute_follow_ends_with(text, n, " muted") && n > 6) return MUTE_ANNOUNCE_MUTED;
    return MUTE_ANNOUNCE_NONE;
}

#endif /* MUTE_FOLLOW_H */
