/*
 * lane_serial -- the `lanes:state` document.
 *
 * A flat, line-oriented text document. Not JSON, even though the file it ends
 * up in is called lanes_<i>.json: the chain formats it on the SPI callback and
 * the shadow UI only ever carries it as an OPAQUE STRING between a get_param
 * and a host_write_file, so neither side ever needs a parser it would have to
 * be given one. The name keeps it beside the set's other state files.
 *
 *   V <version>
 *   L <target> <param> <track> <slot> <loop_start> <loop_len> <note_count> <first_note> <n>
 *   P <phase> <value>
 *   P <phase> <value>
 *
 * TWO RULES THAT ARE NOT OBVIOUS
 * ------------------------------
 * 1. A MISSING first_note IS -1, NEVER 0. `{note_count: 0, first_note: -1}` is
 *    the pattern lane_fingerprint_matches refuses as "no fingerprint was
 *    recorded", and that refusal is the only thing keeping a lane recorded
 *    before the fingerprint existed from binding to any clip whose loop starts
 *    where it does. Note 0 is a real note number, so a zero default is a
 *    CLAIM, and the wrong-clip defect comes back through the file. The three
 *    trailing fields of the L line are therefore optional on read, and absent
 *    means absent.
 *
 * 2. `stale` AND `orphaned` ARE NOT PERSISTED. lane_tick recomputes both from
 *    the live clip every block, and only a MATCH clears `stale` -- so a
 *    persisted `stale` would strand a lane whose clip is present, with no
 *    gesture anywhere that un-strands it. Same for `driving` and the punch
 *    pair, which describe this block and not the content.
 *
 * The header's point count is VALIDATED against the P lines that follow and
 * never used to size anything: a count believed over the data is a buffer
 * overrun waiting to happen, and a count ignored accepts a corrupt file in
 * silence. A disagreement is malformed.
 */
#ifndef LANE_SERIAL_H
#define LANE_SERIAL_H

#include "lane_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LANE_SERIAL_VERSION 1

/* Worst case: one "V n" line, then per lane a header plus LANE_POINTS_MAX
 * points.
 *
 * The two per-lane figures are COUNTED, not eyeballed, because LANE_MAX now
 * spans clips x parameters and the total is what the param-contract ceiling
 * constrains:
 *
 *   header  "L " + target(15) + param(31) + two %d + two %.17g + three %d
 *           = 2+15+1+31+1 + 2*(11+1) + 2*(25+1) + 3*(11+1) = 162  -> 192
 *   point   "P " + %.17g + %.9g + "\n" = 2+25+1+16+1 = 45        ->  48
 *
 * The old figures were 128 and 64: the header one was UNDER its own worst
 * case (a %d of a negative int is 11 characters, not 1) while the point one
 * was 40% over. Both are now above, with the header no longer optimistic.
 *
 * This define is for callers sizing a buffer, not a substitute for a check --
 * the serializer bounds-checks regardless and refuses rather than truncating.
 * lane_serial.c asserts the whole thing stays inside SHADOW_PARAM_VALUE_LEN,
 * which is the real cap on LANE_MAX. */
#define LANE_SERIAL_MAX_BYTES (16 + LANE_MAX * (192 + LANE_POINTS_MAX * 48))

/* Bytes written (excluding the NUL), 0 for an EMPTY STORE, or negative when
 * the buffer is too small.
 *
 * Zero is load-bearing: the shadow UI writes lanes_<i>.json only for a
 * non-empty answer, so a slot with no automation must produce no document at
 * all rather than a well-formed one with no lanes. Negative is distinct from
 * zero because a short write is a failure the caller must not persist -- a
 * positive count for a truncated document is how a good file gets replaced
 * with half of one. */
int lane_store_serialize(const lane_store_t *st, char *buf, int buf_len);

/* 1 on success, 0 on a malformed (or empty, or NULL) document.
 *
 * ALL OR NOTHING. Validates the whole document before touching `st`, so a
 * refusal leaves the store exactly as it was. Half-loading is worse than
 * refusing: the lanes that landed would play against a clip the missing ones
 * were recorded with, and nothing would report it. */
int lane_store_deserialize(lane_store_t *st, const char *doc);

#ifdef __cplusplus
}
#endif
#endif /* LANE_SERIAL_H */
