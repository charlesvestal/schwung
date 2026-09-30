/*
 * Step chance through the chain's own entry points: `chance:` params in, the
 * gate on Move's notes, `chance:state` out and back.
 *
 * What the pure tests cannot see: that the gate reads the instance's CURRENT
 * clip row and phase (the lanes' fields), that an unknown pass from calloc
 * does not read as pass 0, and that only Move's notes (EXTERNAL) are rolled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chain_internal.h"

void chain_set_clip_pass(void *instance, long pass);

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok  %s\n", m); } while (0)

static int note(chain_instance_t *in, int on, int n, int src) {
    uint8_t m[3] = { (uint8_t)(on ? 0x99 : 0x89), (uint8_t)n, (uint8_t)(on ? 100 : 0) };
    return chance_filter(in, m, 3, src);
}

int main(void) {
    chain_instance_t *in = calloc(1, sizeof *in);
    char buf[4096];
    int r12 = -1;
    for (int i = 0; i < sc_count(); i++) { int a, b; if (sc_ratio(i, &a, &b) && a == 1 && b == 2) r12 = i; }

    /* a kick at clip time 0 and a snare at 1.0 on row 2, both 1:2 */
    snprintf(buf, sizeof buf, "2 %d 101 36 0 102 38 1", r12);
    chance_param_set(in, "notes", buf);
    CHECK(sc_store_count(&in->chance) == 2, "chance:notes stores every note it names");
    chance_param_get(in, "of:2:101", buf, sizeof buf);
    CHECK(atoi(buf) == r12, "chance:of reads the condition back");

    /* the clip is playing row 2 at phase 0 */
    in->clip_phase_valid = 1; in->clip_phase_beats = 0.0;
    in->clip_loop_start = 0.0; in->clip_loop_len = 4.0; in->lane_clip_slot = 2;

    /* calloc's pass is UNKNOWN, and an unknown pass plays */
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 1, "no pass pushed yet: plays (not pass 0)");
    note(in, 0, 36, MOVE_MIDI_SOURCE_EXTERNAL);
    chain_set_clip_pass(in, 1);
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 0, "1:2 on pass 1 drops the note-on");
    CHECK(note(in, 0, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 0, "...and its note-off");
    chain_set_clip_pass(in, 2);
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 1, "1:2 on pass 2 plays");
    note(in, 0, 36, MOVE_MIDI_SOURCE_EXTERNAL);

    chance_param_get(in, "stats", buf, sizeof buf);
    CHECK(strcmp(buf, "3 1") == 0, "chance:stats counts matched and dropped");

    /* only Move's notes roll */
    chain_set_clip_pass(in, 1);
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_HOST) == 1, "host-generated MIDI is never rolled");
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_INTERNAL) == 1, "internal MIDI is never rolled");
    /* another row playing: the entries are not this clip's */
    in->lane_clip_slot = 3;
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 1, "a different clip row plays everything");
    in->lane_clip_slot = 2;
    /* stopped: phase unknown */
    in->clip_phase_valid = 0;
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 1, "unknown phase plays");
    in->clip_phase_valid = 1;
    /* Pre mode stands down */
    in->midi_fx_pre_mode = 1;
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 1, "Pre mode never rolls");
    in->midi_fx_pre_mode = 0;

    /* ---- persistence round trip ---- */
    int n = chance_param_get(in, "state", buf, sizeof buf);
    CHECK(n > 0 && strncmp(buf, "SC 1\n", 5) == 0, "chance:state serves the document");
    char doc[4096]; strcpy(doc, buf);
    chance_param_set(in, "clear", "");
    CHECK(sc_store_count(&in->chance) == 0, "chance:clear empties the store");
    n = chance_param_get(in, "state", buf, sizeof buf);
    CHECK(n == 0 && buf[0] == 0, "an empty store serves \"\" (the autosave deletes the file)");
    chance_param_set(in, "state", doc);
    CHECK(sc_store_count(&in->chance) == 2, "chance:state restores it");
    chance_param_set(in, "state", "garbage");
    CHECK(sc_store_count(&in->chance) == 2, "a refused document leaves the store alone");
    CHECK(chance_param_get(in, "nope", buf, sizeof buf) == -1, "an unknown key is a FAILED read, not \"\"");

    /* ---- relocate: Move nudged the snare ---- */
    chance_param_set(in, "move", "2 102 38 1.1");
    in->clip_phase_beats = 1.1; chain_set_clip_pass(in, 1);
    CHECK(note(in, 1, 38, MOVE_MIDI_SOURCE_EXTERNAL) == 0, "a relocated note keeps its condition");

    /* ---- prune: Move deleted the kick ---- */
    chance_param_set(in, "prune", "2 0 4 102");
    chance_param_get(in, "of:2:101", buf, sizeof buf);
    CHECK(atoi(buf) == SC_ALWAYS, "chance:prune drops a note Move no longer has");
    chance_param_get(in, "of:2:102", buf, sizeof buf);
    CHECK(atoi(buf) == r12, "...and keeps one it still has");

    /* ---- FOLLOWING MOVE'S EDITS, through the lanes' verbs ---------------- */
    chance_param_set(in, "clear", "");
    snprintf(buf, sizeof buf, "0 %d 11 36 0", r12);
    chance_param_set(in, "notes", buf);                       /* 1:2 on step 1, row 0 */
    /* a step paste 0.0 -> 1.0, journal id 5, voice-scoped to pitch 36 */
    chance_on_lane_verb(in, "paste_span", "0 0 0 1 0.25 5 v=36");
    in->lane_clip_slot = 0; in->clip_phase_beats = 1.0; chain_set_clip_pass(in, 1);
    CHECK(note(in, 1, 36, MOVE_MIDI_SOURCE_EXTERNAL) == 0, "a pasted step carries its condition (1:2 drops pass 1)");
    note(in, 0, 36, MOVE_MIDI_SOURCE_EXTERNAL);
    chance_on_lane_verb(in, "journal", "undo 5");
    CHECK(sc_store_match(&in->chance, 0, 36, 1.0, 0.0, 4.0) == SC_ALWAYS, "Move's Undo of the paste removes the copy");
    chance_on_lane_verb(in, "journal", "redo 5");
    CHECK(sc_store_match(&in->chance, 0, 36, 1.0, 0.0, 4.0) == r12, "...and Redo puts it back");
    chance_on_lane_verb(in, "journal", "undo 2147483653");   /* high bit: Schwung's own edit */
    CHECK(sc_store_match(&in->chance, 0, 36, 1.0, 0.0, 4.0) == r12, "an own-edit journal id is ignored");
    /* adoption by Move's real id */
    chance_param_set(in, "adopt", "0 555 36 1");
    chance_param_get(in, "of:0:555", buf, sizeof buf);
    CHECK(atoi(buf) == r12, "chance:adopt keys a copied condition to Move's note id");
    /* Double Loop */
    chance_on_lane_verb(in, "paste_span", "0 0 0 4 4 6");
    CHECK(sc_store_match(&in->chance, 0, 36, 4.0, 0.0, 8.0) == r12, "Double Loop copies the conditions onto the new half");
    /* clip copy to slot 2 */
    chance_on_lane_verb(in, "copy_clip", "0 2");
    CHECK(sc_store_match(&in->chance, 2, 36, 0.0, 0.0, 4.0) == r12, "a copied clip carries its conditions");
    /* clip delete + Undo */
    const int before = sc_store_count(&in->chance);
    chance_on_lane_verb(in, "stash", "0 2 9");
    CHECK(sc_store_match(&in->chance, 2, 36, 0.0, 0.0, 4.0) == SC_ALWAYS, "a deleted clip's conditions stop");
    chance_param_get(in, "state", buf, sizeof buf);
    CHECK(strstr(buf, "\n20") == NULL, "parked conditions are not persisted");
    chance_on_lane_verb(in, "unstash", "8 0 2");               /* wrong sid: nothing */
    CHECK(sc_store_match(&in->chance, 2, 36, 0.0, 0.0, 4.0) == SC_ALWAYS, "a stale unstash is refused");
    chance_on_lane_verb(in, "unstash", "9 0 2");
    CHECK(sc_store_match(&in->chance, 2, 36, 0.0, 0.0, 4.0) == r12, "Move's Undo of the delete brings them back");
    CHECK(sc_store_count(&in->chance) == before, "...all of them, once");
    free(in);
    printf(fails ? "FAIL: %d\n" : "PASS: chain chance\n", fails);
    return fails ? 1 : 0;
}
