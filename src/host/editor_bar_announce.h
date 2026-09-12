/*
 * editor_bar_announce — Move's "Bar N" step-editor page announcement.
 *
 * WHY IT MATTERS
 * --------------
 * It is the only EXTERNAL statement of which page Move's step editor is on.
 * The playhead cannot supply that: it is visible exactly when the displayed
 * page contains it, so deriving the page from the playhead assumes the very
 * phase you wanted to check. "Bar N" breaks that circle, which is what makes
 * a whole-bar phase error detectable at all -- the playhead-index comparison
 * is mod 16 steps, i.e. mod one bar, and scores 100% on a lane anchored
 * exactly a bar out.
 *
 * STRICTNESS, for the same reason metronome_announce.h is strict: this is a
 * shared channel. Move announces track names, parameter values and Schwung's
 * own TTS through it, and a loose match on "bar" would fire on a clip called
 * "Bar Fight". The whole normalised string must be "bar <digits>", nothing
 * else -- no prefix, no suffix, no trailing words.
 *
 * A TRUNCATED announcement must NOT parse. "Bar 12" cut to "Bar 1" is a
 * plausible string that names the wrong page, so anything that does not end
 * cleanly is refused rather than half-read.
 *
 * Pure header so tests/host can run it without D-Bus.
 */
#ifndef EDITOR_BAR_ANNOUNCE_H
#define EDITOR_BAR_ANNOUNCE_H

/* Returns the 1-based bar, or 0 if this announcement is not a bar change.
 * Never returns a partial or guessed answer. */
static inline int editor_bar_parse(const char *text)
{
    if (!text) return 0;
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    /* Case-insensitive "bar", then at least one space. */
    if ((p[0] != 'B' && p[0] != 'b')) return 0;
    if ((p[1] != 'A' && p[1] != 'a')) return 0;
    if ((p[2] != 'R' && p[2] != 'r')) return 0;
    p += 3;
    if (*p != ' ' && *p != '\n' && *p != '\t') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return 0;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > 9999) return 0;           /* not a bar number */
        p++;
    }
    /* Nothing but trailing whitespace may follow. "Bar 4 of 8" is a different
     * message and must not be read as "Bar 4". */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '\0') return 0;
    return n > 0 ? n : 0;
}

#endif /* EDITOR_BAR_ANNOUNCE_H */
