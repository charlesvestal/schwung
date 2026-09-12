#include "lane_serial.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* A whole line, refused rather than truncated if it does not fit. The longest
 * legitimate line is an L header (a 15-byte target, a 31-byte param and seven
 * numbers), so anything near this is corruption. */
#define LANE_SERIAL_LINE_MAX 256

/* %.17g round-trips a double exactly and %.9g a float, while %g still prints
 * "3.5" for 3.5 -- so the document stays readable and the phases come back
 * bit-identical rather than merely close. The acceptance criterion is six
 * decimal places; this costs nothing to exceed. */
#define FMT_D "%.17g"
#define FMT_F "%.9g"

int lane_store_serialize(const lane_store_t *st, char *buf, int buf_len) {
    if (!st || !buf || buf_len <= 0) return -1;

    int used = 0;
    for (int i = 0; i < LANE_MAX; i++) if (st->lanes[i].used) { used = 1; break; }
    /* NOTHING, not an empty document. The caller writes a file only for a
     * non-empty answer, so this is what keeps a slot with no automation from
     * leaving one behind -- and from overwriting one. */
    if (!used) { buf[0] = '\0'; return 0; }

    int off = 0;
#define APPEND(...) do {                                                  \
        int _r = snprintf(buf + off, (size_t)(buf_len - off), __VA_ARGS__); \
        /* A short write is a MALFORMED document, and the caller would put  \
         * it in a file. Refuse rather than hand back a positive count. */   \
        if (_r < 0 || _r >= buf_len - off) { buf[0] = '\0'; return -1; }    \
        off += _r;                                                          \
    } while (0)

    APPEND("V %d\n", LANE_SERIAL_VERSION);
    for (int i = 0; i < LANE_MAX; i++) {
        const lane_t *ln = &st->lanes[i];
        if (!ln->used) continue;
        /* stale / orphaned / driving / punch_* are deliberately absent: they
         * are recomputed from the live clip every block, and only a
         * fingerprint MATCH clears `stale`. Writing one down strands the lane
         * on the next load. */
        APPEND("L %s %s %d %d " FMT_D " " FMT_D " %d %d %d\n",
               ln->target, ln->param, ln->track, ln->slot,
               ln->fp.loop_start, ln->fp.loop_len,
               ln->fp.note_count, ln->fp.first_note, ln->n);
        for (int k = 0; k < ln->n && k < LANE_POINTS_MAX; k++)
            APPEND("P " FMT_D " " FMT_F "\n",
                   ln->pts[k].phase, (double)ln->pts[k].value);
    }
#undef APPEND
    return off;
}

/* One L header as parsed. `declared_n` is -1 when the line did not carry one
 * (an older or hand-written document): absent is a third answer, and the
 * count is only ever a cross-check anyway. */
typedef struct {
    char target[16];
    char param[32];
    int  track;
    int  slot;
    lane_fingerprint_t fp;
    int  declared_n;
    int  seen_n;
    double last_phase;
} lane_hdr_t;

/* Does the lane that just ended agree with its own header? */
static int hdr_close_ok(const lane_hdr_t *h) {
    if (h->seen_n > LANE_POINTS_MAX) return 0;
    /* The count is CHECKED, never used to size or bound anything -- believing
     * it over the data is a buffer overrun waiting to happen, and ignoring it
     * accepts a corrupt file in silence. */
    if (h->declared_n >= 0 && h->declared_n != h->seen_n) return 0;
    return 1;
}

static int parse_hdr(const char *line, lane_hdr_t *h) {
    char t[LANE_SERIAL_LINE_MAX], p[LANE_SERIAL_LINE_MAX];
    double ls = 0.0, ll = 0.0;
    int track = 0, slot = 0, nc = 0, fn = -1, dn = -1;
    /* THE THREE TRAILING FIELDS ARE OPTIONAL, and a missing first_note is -1.
     * Zero is note 0, a real note number, so it would turn "no fingerprint was
     * recorded" into a claim that lane_fingerprint_matches believes -- and the
     * wrong-clip binding this whole design exists to prevent comes back
     * through the file. `fn` is initialised to -1 above AND only assigned when
     * the field was actually present. */
    int got = sscanf(line, "L %255s %255s %d %d %lf %lf %d %d %d",
                     t, p, &track, &slot, &ls, &ll, &nc, &fn, &dn);
    if (got < 6) return 0;
    if (got < 7) nc = 0;
    if (got < 8) fn = -1;
    if (got < 9) dn = -1;
    /* A key too long for lane_t's storage is REFUSED, never truncated: two
     * over-length keys would collide onto one stored string and bind a lane to
     * the wrong parameter. Mirrors lane_store.c's own rule. */
    if (strlen(t) >= sizeof(h->target) || strlen(p) >= sizeof(h->param)) return 0;
    if (!isfinite(ls) || !isfinite(ll)) return 0;
    memset(h, 0, sizeof(*h));
    snprintf(h->target, sizeof(h->target), "%s", t);
    snprintf(h->param, sizeof(h->param), "%s", p);
    h->track = track;
    h->slot = slot;
    h->fp.loop_start = ls;
    h->fp.loop_len = ll;
    h->fp.note_count = nc;
    h->fp.first_note = fn;
    h->declared_n = dn;
    h->seen_n = 0;
    h->last_phase = -1.0;
    return 1;
}

/* One walk of the document. `apply` 0 validates and writes nothing; 1 fills
 * `st`. Called twice, which is what makes a refusal leave the store untouched
 * without a 19 KB temporary on the SPI callback's stack. */
static int parse_doc(lane_store_t *st, const char *doc, int apply) {
    char seen[LANE_MAX][sizeof(((lane_t *)0)->target) +
                        sizeof(((lane_t *)0)->param) + 2];
    int nseen = 0;
    lane_hdr_t h;
    int have_hdr = 0;
    int lane_idx = 0;
    lane_t *cur = 0;

    const char *p = doc;
    while (*p) {
        char line[LANE_SERIAL_LINE_MAX];
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) return 0;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + len;
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len == 0) continue;

        if (line[0] == 'V') {
            int v = 0;
            if (sscanf(line, "V %d", &v) != 1) return 0;
            /* A document from the future cannot be read safely, and guessing
             * is the one answer that can be confidently wrong. */
            if (v < 1 || v > LANE_SERIAL_VERSION) return 0;
            continue;
        }

        if (line[0] == 'L') {
            if (have_hdr && !hdr_close_ok(&h)) return 0;
            if (!parse_hdr(line, &h)) return 0;
            have_hdr = 1;
            if (lane_idx >= LANE_MAX) return 0;   /* dropping lanes silently
                                                   * loses recorded automation */
            if (!apply) {
                char key[sizeof(seen[0])];
                snprintf(key, sizeof(key), "%s\t%s", h.target, h.param);
                for (int i = 0; i < nseen; i++)
                    if (strcmp(seen[i], key) == 0) return 0;  /* two lanes, one
                                                               * key: lane_find
                                                               * could only ever
                                                               * reach the first */
                snprintf(seen[nseen++], sizeof(seen[0]), "%s", key);
            } else {
                cur = &st->lanes[lane_idx];
                memset(cur, 0, sizeof(*cur));
                cur->used = 1;
                snprintf(cur->target, sizeof(cur->target), "%s", h.target);
                snprintf(cur->param, sizeof(cur->param), "%s", h.param);
                cur->track = h.track;
                cur->slot = h.slot;
                cur->fp = h.fp;
            }
            lane_idx++;
            continue;
        }

        if (line[0] == 'P') {
            if (!have_hdr) return 0;   /* a point with no lane to belong to */
            double phase = 0.0;
            double value = 0.0;
            /* Both fields or neither. A phase with a defaulted value plants
             * 0.0 on a breakpoint the user never played. */
            if (sscanf(line, "P %lf %lf", &phase, &value) != 2) return 0;
            if (!isfinite(phase) || phase < 0.0 || !isfinite(value)) return 0;
            /* lane_eval walks pts[] assuming ascending phase, so an unsorted
             * document would evaluate to the wrong curve rather than to an
             * error. */
            if (phase < h.last_phase) return 0;
            h.last_phase = phase;
            h.seen_n++;
            if (h.seen_n > LANE_POINTS_MAX) return 0;
            if (apply && cur) {
                cur->pts[cur->n].phase = phase;
                cur->pts[cur->n].value = (float)value;
                cur->n++;
            }
            continue;
        }

        return 0;   /* an unknown line is corruption, not something to skip */
    }

    if (have_hdr && !hdr_close_ok(&h)) return 0;
    return have_hdr ? 1 : 0;
}

int lane_store_deserialize(lane_store_t *st, const char *doc) {
    if (!st || !doc || !*doc) return 0;
    /* VALIDATE THE WHOLE DOCUMENT FIRST. Half-loading is worse than refusing:
     * the lanes that landed would play against a clip the missing ones were
     * recorded with, and nothing would report it. */
    if (!parse_doc(st, doc, 0)) return 0;
    lane_store_reset(st);
    return parse_doc(st, doc, 1);
}
