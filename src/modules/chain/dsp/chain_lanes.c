/*
 * chain_lanes -- clip-associated automation lanes, the chain-side half.
 *
 * The pure model is src/host/lane_store.c; this file is the glue that knows
 * about chain_instance_t, parameter types and the mod bus. A lane is an
 * ABSOLUTE source: chain_mod_emit_override substitutes its value for the
 * knob's base, so LFO offsets still sum on top of whatever the lane played.
 *
 * RT: lane_tick runs on the SPI callback -- from render_block, and from the
 * "mod:tick" branch on a silent slot. No allocation, no file I/O, no locks,
 * no logging, and every loop is bounded by LANE_MAX.
 */
#include "chain_internal.h"

#include <stdio.h>
#include <math.h>   /* isfinite, for the p-lock phase */

/* One source id per lane, so the mod bus can tell two lanes apart and clear
 * one without disturbing the other -- and so a lane's own release names only
 * itself. target is 16 bytes and param 32 (lane_store.h), both of which
 * lane_alloc REFUSES to truncate, so this cannot lose a lane's identity. */
static void lane_source_id(const lane_t *ln, char *buf, int len) {
    snprintf(buf, len, "lane:%s:%s", ln->target, ln->param);
}

/* Hand the parameter back to the user's knob. chain_mod_emit_override with
 * enabled=0 drops this source and restores the base with a forced write. */
static void lane_release_one(chain_instance_t *inst, lane_t *ln) {
    char sid[64];
    lane_source_id(ln, sid, sizeof(sid));
    chain_mod_emit_override(inst, sid, ln->target, ln->param, 0.0f, 0);
    ln->driving = 0;
}

/* END EVERY RECORDING PASS.
 *
 * A pass without an end is worse than no pass: the first write of the NEXT
 * take would erase the swept span back to wherever the last take happened to
 * stop. Called when Record goes out and whenever the lane stops being the one
 * playing -- separate from lane_release_all, which skips lanes that are not
 * driving and is also called by paths (a state load, a clear) that empty the
 * store anyway. */
void lane_record_end_all(chain_instance_t *inst) {
    if (!inst) return;
    for (int i = 0; i < LANE_MAX; i++)
        lane_record_end(&inst->lanes.lanes[i]);
}

/* Remember the whole store so the next edit can be taken back.
 *
 * Called before DISCRETE edits and once at the start of a recording pass --
 * never per recorded point, which would be a 37 KB memcpy per breakpoint of a
 * sweep on the SPI callback. */
void lane_undo_take(chain_instance_t *inst) {
    if (!inst) return;
    memcpy(&inst->lanes_undo, &inst->lanes, sizeof(inst->lanes_undo));
    inst->lanes_undo_valid = 1;
}

void lane_release_all(chain_instance_t *inst) {
    if (!inst) return;
    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &inst->lanes.lanes[i];
        /* `driving` is what makes this ONCE-ONLY. Releasing unconditionally
         * every block would re-write the base over the very knob turn the
         * first release handed back, so the knob would be dead for as long as
         * the transport was stopped. */
        if (!ln->used || !ln->driving) continue;
        lane_release_one(inst, ln);
    }
}

void lane_tick(chain_instance_t *inst) {
    if (!inst) return;

    /* NO PHASE MEANS UNKNOWN, AND UNKNOWN IS NOT ZERO. Release anything we
     * are driving -- once -- so the parameter returns to the user's knob
     * rather than freezing wherever the clip happened to stop. Reading this
     * as phase 0 instead would drive the first breakpoint with the transport
     * stopped, which is a parameter moving on its own with nothing playing. */
    if (!inst->clip_phase_valid || !(inst->clip_loop_len > 0.0) ||
        !(inst->clip_loop_start >= 0.0)) {
        lane_release_all(inst);
        /* And the passes end here. With no phase there is no swept span to
         * continue from, so the next write that does have one must be a first
         * write rather than a sweep from a phase measured before the gap. */
        lane_record_end_all(inst);
        return;
    }

    for (int i = 0; i < LANE_MAX; i++) {
        lane_t *ln = &inst->lanes.lanes[i];
        if (!ln->used) continue;

        /* A lane belongs to a POSITION, not to the transport. A different
         * clip playing must silence it: a lane playing the wrong clip's
         * automation is worse than no lane at all, and nothing on screen
         * would explain it. (Fingerprint matching -- the same clip position
         * holding different content -- is Task 6's, through ln->stale, which
         * lane_eval already refuses.) */
        if (ln->track != inst->lane_track || ln->slot != inst->lane_clip_slot) {
            if (ln->driving) lane_release_one(inst, ln);
            /* This lane's clip stopped being the one playing, so its pass is
             * over whatever Record is doing. Otherwise coming back to the clip
             * later resumes a sweep from a phase in a different take. */
            lane_record_end(ln);
            continue;
        }

        /* THE SAME POSITION CAN HOLD A DIFFERENT CLIP. The check above proves
         * only that the transport is on this lane's grid position; whether the
         * clip there is the one the lane was recorded against is what the
         * fingerprint answers, because Move's clips carry no identity at all.
         *
         * Both directions, and both flags. A match un-stales AND un-orphans:
         * the clip coming back is an undo, and there is no gesture in the UI
         * that would otherwise un-strand a lane. A mismatch only ever sets
         * `stale` -- `orphaned` is a statement about the clip's EXISTENCE, and
         * only the worker's before/after pair can make it (a single parse
         * cannot tell a deleted clip from one Move has not saved yet).
         *
         * Gated on clip_fp_valid: no fingerprint is "could not tell", which is
         * neither a match nor a mismatch, so nothing is marked either way. */
        if (inst->clip_fp_valid) {
            /* A LANE RECORDED BLIND IS ADOPTED HERE, not marked stale.
             *
             * Move writes a new clip to Song.abl ~10 s after it is made, and
             * inside that window there are no notes to fingerprint and no
             * loop.start to anchor to -- so a take goes down against an
             * assumed origin of 0 with the ABSENT fingerprint, which is the
             * same fact as "this lane still needs the real origin".
             *
             * This is the moment both unknowns are answered: the clip has
             * appeared, so its notes identify it and its loop.start places
             * the take. Without this the next block would mark the lane stale
             * -- correctly, by the letter of the rule, and the user's
             * automation would go silent about ten seconds after they
             * recorded it, with nothing on screen to explain why.
             *
             * The hazard is adopting the WRONG clip, and the guards are: the
             * lane's own fingerprint must be absent (lane_adopt_fingerprint
             * refuses otherwise, so an identified lane can never be
             * re-labelled), and we are already past the position check above,
             * so this IS the clip playing at the lane's own (track, slot).
             * Nothing here infers a clip from anything but the position it
             * was recorded at. */
            if (lane_fp_absent(&ln->fp))
                lane_adopt_fingerprint(ln, &inst->clip_fp);

            if (lane_fingerprint_matches(ln, &inst->clip_fp)) {
                ln->stale = 0;
                ln->orphaned = 0;
            } else if (!ln->orphaned && !lane_fp_absent(&ln->fp)) {
                /* AN EDIT, NOT A REPLACEMENT -- so re-stamp rather than go
                 * stale.
                 *
                 * The fingerprint is note count plus first note, so ADDING OR
                 * DELETING ONE NOTE broke it and the clip's automation went
                 * silent. Measured on hardware: a lane driving at 0.9 with
                 * `:modulated` 1 read 0.47 and 0 after a single step press.
                 * Every editing gesture does this -- add a note, delete one,
                 * copy a bar -- which is most of what anyone does to a clip,
                 * and nothing on screen explains it.
                 *
                 * IDENTITY IS CONTINUITY, and the fingerprint is the tiebreak
                 * for the discontinuous case. A clip that was REPLACED went
                 * through a deletion, and the worker's before/after parse sets
                 * `orphaned` for exactly that -- so a mismatch while NOT
                 * orphaned is the same clip, edited. The guard that matters is
                 * kept: an orphaned position stays silent until the clip it
                 * was recorded against comes back, which is what makes an undo
                 * restore automation instead of a stranger inheriting it.
                 *
                 * The hole this leaves, stated plainly: a clip deleted and
                 * recreated in the same slot INSIDE one save window (~10 s)
                 * shows no deletion to the worker, so the lane treats it as an
                 * edit and plays on the new clip. That is a worse failure than
                 * silence -- but it is rarer than editing a note, which is the
                 * failure it replaces, and Move's own Copy lands in the next
                 * FREE slot rather than over an existing clip.
                 *
                 * A lane with the ABSENT fingerprint is excluded: it was never
                 * identified, so there is nothing to call an edit OF. Those go
                 * through lane_adopt_fingerprint, which demands that THIS
                 * session recorded them blind -- otherwise a placeholder
                 * loaded from disk would bind to the first clip it met, which
                 * is the one outcome this design has always refused. */
                ln->fp = inst->clip_fp;
                ln->stale = 0;
                ln->adopted++;
            } else {
                ln->stale = 1;
            }
        }

        /* An unarmed knob turn punches through until the loop comes round --
         * otherwise, under an absolute lane, turning a knob does nothing
         * audible and reads as a broken encoder. Set in Task 5.
         *
         * The EXPIRY is evaluated unconditionally, ahead of the suppression
         * below, so a punch cannot outlive its wrap just because a recording
         * pass was suppressing the lane over the same blocks. */
        if (ln->punch_until_wrap && inst->clip_phase_beats < ln->punch_phase)
            ln->punch_until_wrap = 0;

        /* A LIVE RECORDING PASS SILENCES ITS OWN LANE.
         *
         * A lane is an ABSOLUTE source, so while the user is recording over
         * one the old curve is written on top of the knob every block and the
         * take is inaudible while it is being made -- diagnosed on hardware.
         * Task 5 had the reasoning inverted ("an armed turn IS the lane", so
         * keep driving); an armed turn is exactly when the lane must yield.
         *
         * Bounded to the pass's own travel rather than to `rec_active`, from
         * lane_pass_live_at -- the SAME computation lane_record_point erases
         * with, so the silent region and the erased region are one region. The
         * rest of the loop keeps playing, which is what makes this
         * punch-in/punch-out instead of a recording mode. */
        const int pass_live = lane_pass_live_at(ln, inst->clip_phase_beats,
                                                inst->clip_loop_start,
                                                inst->clip_loop_len);

        /* AND THE PASS ENDS HERE, which is punch-OUT. The transport has
         * carried a whole LANE_PASS_GAP_BEATS past the last write with no
         * further one, so the gesture is over; leaving `rec_active` set would
         * make the lane go silent at that phase on every later loop, because
         * the transport comes back round through it. lane_record_point already
         * refuses to erase across a gap that wide, so nothing is lost. */
        if (!pass_live && ln->rec_active) lane_record_end(ln);

        if (ln->punch_until_wrap || pass_live) {
            /* Through the ordinary release, which is ONCE-ONLY (`driving`):
             * re-emitting it every block would rewrite the base over the very
             * next knob detent, which is this same defect in a quieter form. */
            if (ln->driving) lane_release_one(inst, ln);
            continue;
        }

        chain_param_info_t *pinfo = find_param_by_key(inst, ln->target, ln->param);
        /* The module was swapped out from under the lane. Skip it and keep
         * the lane: the user's automation is not the swap's to delete, and
         * the same module coming back must find its lane still there. */
        if (!pinfo) continue;

        /* The TYPE decides how the curve reads between breakpoints, and it
         * comes from the module's own metadata rather than from anything the
         * lane stored -- a lane recorded before a module changed a param from
         * float to enum must not keep interpolating across its options. */
        const int stepped = (pinfo->type == KNOB_TYPE_INT ||
                             pinfo->type == KNOB_TYPE_ENUM);

        float v = 0.0f;
        if (!lane_eval(ln, inst->clip_phase_beats, inst->clip_loop_start,
                       inst->clip_loop_len,
                       stepped, &v)) {
            /* "Nothing to say" -- empty, stale or orphaned. That is NOT the
             * value 0.0, so it releases rather than writing anything. */
            if (ln->driving) lane_release_one(inst, ln);
            continue;
        }

        char sid[64];
        lane_source_id(ln, sid, sizeof(sid));
        chain_mod_emit_override(inst, sid, ln->target, ln->param, v, 1);
        ln->driving = 1;
    }
}

/* The clip's fingerprint as it is RIGHT NOW.
 *
 * With no clip known it is the ABSENT fingerprint -- {0, -1}, not all-zero.
 * A zeroed first_note is note 0, a real note number, so an all-zero
 * fingerprint is a claim rather than a gap: it would match any clip at
 * loop_start 0 whose earliest note happened to be 0. {0, -1} is the pattern
 * lane_fingerprint_matches refuses outright, so a lane recorded against an
 * unknown clip is stale until it is re-recorded, which is the only answer that
 * cannot be confidently wrong. */
void lane_current_fingerprint(chain_instance_t *inst, lane_fingerprint_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->first_note = -1;
    if (inst && inst->clip_fp_valid) *out = inst->clip_fp;
}

/* IS A RECORDING PASS THE THING THIS WRITE IS DOING?
 *
 * The exact condition the record branch of lane_on_set_param takes, lifted so
 * that it can be ASKED as well as taken. The host has to know it: a component
 * write made while a step is held is also a p-lock (see
 * shadow_lanes_plock_from_write), and a p-lock writes a RECTANGLE into the
 * same lane a sweep is being recorded into -- so a stale or incidental held
 * step would punch stepped points through a take. Charles reported exactly
 * that, which is why the write-time p-lock was reverted once already.
 *
 * The two gestures are mutually exclusive by construction now, and the
 * condition lives HERE, once. Restating it host-side would be a second copy
 * of the recording predicate, free to disagree with this one -- the failure
 * mode this feature has already paid for elsewhere (`synth:last_note`, the
 * transport grid).
 *
 * Armed WITHOUT a phase is deliberately not recording: such a write records
 * nothing, so it is still free to be a p-lock, which needs no phase. */
static int lane_is_recording(const chain_instance_t *inst) {
    return inst && inst->lane_armed && inst->clip_phase_valid &&
           inst->clip_loop_len > 0.0;
}

/* A parameter write arrived from the UI.
 *
 * THIS IS NOT CALLED BY PLAYBACK. chain_mod_set_param_string writes the
 * sub-plugin's set_param directly and never re-enters v2_set_param, which is
 * the only caller of this function, so a lane cannot record its own output.
 * That is structural rather than a flag, and the playback-does-not-record
 * assertion in the unit test is what keeps it true -- if it ever stops being
 * true the lane compounds its own curve every loop, silently and worse each
 * bar, which nothing on screen would explain.
 *
 * Phase is sampled HERE -- on the callback, at the moment of the write --
 * rather than at UI frame time, because a frame is ~23 ms and a knob sweep is
 * faster than that, so frame-time phase would quantize a sweep into steps.
 *
 * RT: no allocation (lane_alloc hands back a slot of a fixed array), no I/O,
 * no locks, and every loop inside is bounded by LANE_MAX / LANE_POINTS_MAX. */
void lane_on_set_param(chain_instance_t *inst, const char *target,
                       const char *param, const char *val) {
    if (!inst || !target || !param || !val) return;

    /* Only a parameter the module actually declares can be automated: the
     * type is what decides stepped-vs-linear on playback, and a lane with no
     * metadata behind it would interpolate across an enum's options. */
    chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
    if (!pinfo) return;
    const float v = dsp_value_to_float(val, pinfo, pinfo->default_val);

    /* ARMED AND THE PHASE IS KNOWN is the only state that records. An armed
     * write with no phase records NOTHING -- "we could not tell where in the
     * clip we are" is a third answer, and writing it at 0.0 would plant a
     * breakpoint on a downbeat the user never played. */
    if (lane_is_recording(inst)) {
        lane_fingerprint_t fp;
        lane_current_fingerprint(inst, &fp);
        lane_t *ln = lane_alloc(&inst->lanes, target, param,
                                inst->lane_track, inst->lane_clip_slot, &fp);
        /* A TAKE RECORDED IN THE BLIND WINDOW IS MARKED AS SUCH. No
         * fingerprint while the phase is valid means Move has not written
         * this clip to Song.abl yet (~10 s), so the geometry we are recording
         * against is provisional: an assumed origin of 0 and a length read
         * off the step editor's own bar strip. `origin_pending` is what lets
         * lane_tick re-origin and identify it in one step when the clip
         * appears -- and it is scoped to a lane THIS session created, so a
         * placeholder loaded from disk can never take a stranger's identity.
         * Set on the existing lane too: a second armed write in the same
         * window must not leave the first one's flag behind. */
        if (ln && !inst->clip_fp_valid && lane_fp_absent(&ln->fp))
            ln->origin_pending = 1;
        /* Store full, or a target/param too long for lane_t's fields, which
         * lane_alloc REFUSES rather than truncating -- a truncated key would
         * name a lane the user can neither see nor clear. Nothing is recorded
         * and nothing pretends to have been. */
        if (!ln) return;
        /* RECORD, not write: a second pass over an existing lane must erase
         * the span it sweeps rather than interleave with it. lane_write's
         * thinning window is ~5 ms and cannot do that job -- see
         * LANE_MIN_POINT_BEATS. */
        /* hold = 0: a recorded knob sweep IS a slope. A step p-lock is the
         * gesture that writes a rectangle. */
        lane_record_point(ln, inst->clip_phase_beats, v,
                          inst->clip_loop_start, inst->clip_loop_len, 0);
        /* AND HAND THE PARAMETER BACK, for the same reason the unarmed branch
         * below does: an active override makes v2_set_param re-apply base+mod
         * and RETURN, so this write would never reach the plugin at all -- the
         * knob inaudible on the very detent that starts the take. lane_tick's
         * pass_live check keeps it released for as long as the pass runs; this
         * only makes it happen on the write rather than a block later.
         *
         * `punch_until_wrap` IS DELIBERATELY LEFT ALONE. Task 5 cleared it
         * here on the reasoning that "an armed turn IS the lane", i.e. that
         * the lane should keep driving through a take -- which is the defect
         * above, stated as a comment. The unarmed punch and the armed pass are
         * two different mechanisms that happen to want the same thing (the
         * lane silent), and sharing one flag between them would make either
         * one's lifetime unreadable: the punch ends at the wrap, the pass ends
         * when the turning stops. Both suppress independently, so an open
         * punch costs nothing here. */
        if (ln->driving) lane_release_one(inst, ln);
        return;
    }

    /* Unarmed (or phaseless) under an existing lane: hand the parameter to the
     * knob until the loop comes round. Without this, an absolute lane rewrites
     * the same target every block and the encoder is inaudible -- which reads
     * as broken hardware, not as automation. */
    lane_t *ln = lane_find(&inst->lanes, target, param,
                           inst->lane_track, inst->lane_clip_slot);
    if (!ln || !ln->used) return;
    /* Only a known phase can say where the punch ends. With no phase the
     * release below is still right (the knob must be heard) but there is no
     * wrap to arm against, and lane_tick releases on a lost phase anyway. */
    if (inst->clip_phase_valid) {
        ln->punch_until_wrap = 1;
        ln->punch_phase = inst->clip_phase_beats;
    }
    if (ln->driving) lane_release_one(inst, ln);
}

/* ---- persistence: the `lanes:state` document ---------------------------
 *
 * Both of these run on the SPI callback, like every other module entry point.
 * Formatting 16 lanes x 64 points is bounded work with no allocation and no
 * I/O -- which is the whole reason the chain only ever formats a STRING and
 * the shadow UI does the file I/O. There is no fopen on this side of the seam
 * and there must never be one.
 */

/* Bytes written, 0 for "this slot has no automation" (the UI then writes no
 * file at all), or -1 when the host's buffer is too small -- which the UI
 * reads as a FAILED read, not as an empty one, so it cannot truncate a good
 * lanes_<i>.json with half a document. */
/* Arm or disarm recording, pushed from the shim on CHANGE only.
 *
 * The shim decodes this from Move's Record LED (src/host/rec_arm.h): solid at
 * full brightness means Move is capturing, and lanes record in exactly that
 * window -- an animation channel is armed or counting in and records nothing.
 *
 * DISARMING RELEASES NOTHING and clears nothing. A lane that was just recorded
 * must keep driving its parameter the moment Record goes out, or every take
 * would end by handing the parameter back to wherever the knob happens to sit.
 * lane_tick owns playback and reads `lane_armed` only to decide whether a turn
 * is a write; the transition itself is not an event anything needs. */
void lane_set_armed(chain_instance_t *inst, int armed) {
    if (!inst) return;
    /* It releases nothing and clears nothing (above) -- but it does END THE
     * RECORDING PASS. That is not a release: it only means the next armed
     * turn is the first write of a new take, so it cannot erase the span
     * between where the last take stopped and where this one starts. */
    if (!armed) lane_record_end_all(inst);
    inst->lane_armed = armed ? 1 : 0;
}

int lane_serve_state(chain_instance_t *inst, char *buf, int buf_len) {
    if (!inst || !buf || buf_len <= 0) return -1;
    return lane_store_serialize(&inst->lanes, buf, buf_len);
}

/* Replace the store from a document.
 *
 * RELEASES FIRST. The mod bus holds one override source per DRIVING lane,
 * named "lane:<target>:<param>"; dropping the store without releasing leaves
 * those sources asserted for lanes that no longer exist, so the parameters
 * they were driving stick wherever the outgoing set left them and no gesture
 * hands them back.
 *
 * `stale` and `orphaned` are not in the document and are not set here: the
 * next lane_tick recomputes both from the live clip, which is the only thing
 * that can tell a lane whose clip is present from one whose clip is gone. A
 * lane whose fingerprint no longer matches therefore loads neutral and is
 * marked stale on the first block with a clip fingerprint -- silent and
 * retained, never guessed at.
 *
 * A malformed document leaves the store as it was (lane_store_deserialize is
 * all-or-nothing), so a corrupt file loses nothing that is already loaded. */
void lane_apply_state(chain_instance_t *inst, const char *doc) {
    if (!inst || !doc) return;
    lane_release_all(inst);
    lane_store_deserialize(&inst->lanes, doc);
}

/* ---- the one "lanes:" dispatch ----------------------------------------
 *
 * Every lane key arrives here, from a SINGLE branch in v2_set_param /
 * v2_get_param. One branch rather than one per key because chain_host.c is
 * pinned at 2900 lines and was sitting two under it: a per-key ladder there
 * would have made the next lane key a choice between the pin and the feature.
 * This also puts the lane keys beside the code that implements them.
 *
 * `sub` is the key PAST "lanes:", so nothing here restates the prefix.
 *
 * RT: both run on the SPI callback like every other module entry point. No
 * allocation, no I/O, no locks, no logging, and every loop is LANE_MAX-bounded.
 */
void lane_param_set(chain_instance_t *inst, const char *sub, const char *val) {
    if (!inst || !sub) return;

    /* The whole store as one opaque document. */
    if (strcmp(sub, "state") == 0) {
        lane_apply_state(inst, val ? val : "");
        return;
    }

    /* Move's Record button, pushed by the shim on CHANGE only. */
    if (strcmp(sub, "armed") == 0) {
        lane_set_armed(inst, val && atoi(val) != 0);
        return;
    }

    /* Throw the slot's automation away.
     *
     * RELEASES FIRST, for the reason lane_apply_state already had to solve:
     * the mod bus holds one override source per DRIVING lane, so emptying the
     * store on its own leaves those sources asserted for lanes that no longer
     * exist. Every parameter they were driving then sticks wherever the lanes
     * left it and NO GESTURE HANDS IT BACK -- a dead knob with nothing on
     * screen to explain it. Reusing lane_release_all rather than re-deriving
     * the restore keeps one release path.
     *
     * The COUNT is kept for `lanes:cleared`, because a clear that reports
     * success without one is indistinguishable from a clear that cleared
     * nothing -- the same reason the recall snapshot counts its skipped
     * positions. Written unconditionally, so a second press answers 0 rather
     * than repeating the first take's number.
     *
     * Guarded on a non-zero value: a stray `lanes:clear=0` must not throw
     * away a set's automation. */
    /* A STEP P-LOCK: "<target> <param> <phase> <value>".
     *
     * The value carries a PHASE, not a bar and a step, because the chain knows
     * nothing about bars, grids or time signatures -- those live host-side
     * (clip_regions.h, step_plock.h) and teaching this side about them would
     * be a second model of Move's editor. The chain's job is what it already
     * does for every other lane write: place a point in clip time.
     *
     * IT IS A RECTANGLE (hold = 1). Setting a value ON a step is not a slope
     * towards the next one.
     *
     * AND ITS PHASE IS NEVER PROVISIONAL, which is why `origin_pending` is not
     * set here even when the clip is unidentified: a p-lock's phase comes from
     * the BAR NUMBER on Move's own strip, so it is true clip time already,
     * while a recorded sweep's phase comes from the transport against a loop
     * origin we may only have assumed. Re-origining a p-lock later would move
     * it off the step the user pressed.
     *
     * It does NOT require a phase, a running transport or a known clip
     * length: the gesture works while stopped (which is its whole advantage
     * over a live pass), and a p-lock on a bar outside the current loop
     * window is legitimate -- Move's strip shows those bars, and a point
     * outside the window is dormant rather than wrong. What it does require is
     * a clip POSITION to key the lane to, and a parameter the module
     * declares. */
    /* REMOVE WHAT IS LOCKED ON ONE STEP: "<phase>" for every lane of this
     * clip, or "<phase> <target> <param>" for one of them.
     *
     * THE GRAIN THAT WAS MISSING. `clear`, `clear_clip`, `clear_param` and
     * `clear_target` all take a whole lane or more, so a single bad p-lock
     * could not be removed at all -- you had to throw away the parameter's
     * entire automation to get rid of one step. That is the gap this closes.
     *
     * A POINT IS "ON" THE STEP within LANE_MIN_POINT_BEATS, the same window
     * lane_write replaces in, so what this removes is exactly what a p-lock
     * there would have overwritten -- the two verbs agree about what "this
     * step" means by sharing the constant rather than by matching.
     *
     * IT TAKES A RECORDED POINT TOO, if one happens to sit on the step. The
     * alternative -- only `hold` points -- would make the gesture refuse
     * exactly where a sweep crosses a step the user can see and wants clear,
     * and "nothing happened" is worse than a curve with one fewer breakpoint;
     * the span either side interpolates across the gap.
     *
     * A LANE EMPTIED THIS WAY IS FREED, and its override released first: the
     * point of clearing the last lock on a parameter is that the knob gets it
     * back, and a used-but-empty lane would keep driving the value it last
     * computed.
     *
     * Undo takes the whole store, as the other clear verbs do, so a mis-aimed
     * clear is one `lanes:undo` away. */
    if (strcmp(sub, "clear_point") == 0) {
        inst->lanes_last_cleared = 0;
        if (!val) return;
        char target[16] = {0}, param[32] = {0};
        double phase = 0.0;
        const int got = sscanf(val, "%lf %15s %31s", &phase, target, param);
        if (got < 1 || !isfinite(phase) || phase < 0.0) return;
        const int one = (got == 3);
        if (inst->lane_track < 0 || inst->lane_clip_slot < 0) return;
        lane_undo_take(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            lane_t *ln = &inst->lanes.lanes[i];
            if (!ln->used) continue;
            if (!lane_is_for_clip(ln, inst->lane_track, inst->lane_clip_slot)) continue;
            if (one && (strcmp(ln->target, target) != 0 ||
                        strcmp(ln->param, param) != 0)) continue;
            int w = 0;
            for (int k = 0; k < ln->n; k++) {
                if (fabs(ln->pts[k].phase - phase) < LANE_MIN_POINT_BEATS) { n++; continue; }
                ln->pts[w++] = ln->pts[k];
            }
            if (w == ln->n) continue;               /* nothing on this step */
            ln->n = w;
            if (ln->n == 0) {
                if (ln->driving) lane_release_one(inst, ln);
                lane_clear_one(ln);
            }
        }
        inst->lanes_last_cleared = n;
        return;
    }

    /* WHAT DOES A LANE HOLD AT THIS PHASE? "<target> <param> <phase>" in,
     * `lanes:probe` read back out.
     *
     * The question behind "hold a step and see the value locked on it". It is
     * a SET because a GET cannot carry arguments -- a param key is one token
     * and this asks about three things -- and the answer is stashed rather
     * than returned for the same reason. One question at a time, on the SPI
     * callback, which is the only thread that serves params.
     *
     * IT ANSWERS THE CURVE, not the point store: `lane_eval` is what will
     * actually play at that phase, and a step showing anything else would be
     * a promise the playback does not keep. `exact` reports separately
     * whether a point SITS there -- within LANE_MIN_POINT_BEATS, the same
     * window lane_write replaces in, so "exact" means "turning here edits
     * this point" and nothing else.
     *
     * The TYPE comes from the module's metadata, like everywhere else, so a
     * probe reads an enum as a step and a float as a slope. */
    if (strcmp(sub, "probe") == 0) {
        inst->lanes_probe_have = 0;
        inst->lanes_probe_exact = 0;
        inst->lanes_probe_value = 0.0f;
        inst->lanes_probe_stepped = 0;
        if (!val) return;
        char target[16] = {0}, param[32] = {0};
        double phase = 0.0, lo = 0.0, len = 0.0;
        /* THE WINDOW IS THE CALLER'S TO NAME, and that is not a convenience.
         * lane_eval answers nothing for a loop_len of 0, and the instance's
         * live geometry IS 0 whenever the transport is stopped -- which is
         * when step editing is mostly done, so a probe using it answered
         * "nothing locked here" for every p-lock on a stopped clip. The host
         * knows the clip's length from Move's own file and strip (it computes
         * it for the step->phase translation already), so it passes it. Two
         * arguments, both optional: a caller that has no window falls back to
         * the live one, which is right while something is playing. */
        int got = sscanf(val, "%15s %31s %lf %lf %lf", target, param, &phase,
                         &lo, &len);
        if (got < 3) return;
        if (got < 5 || !isfinite(lo) || !isfinite(len) || len <= 0.0) {
            lo = inst->clip_loop_start;
            len = inst->clip_loop_len;
        }
        if (!isfinite(phase) || phase < 0.0) return;
        if (inst->lane_track < 0 || inst->lane_clip_slot < 0) return;
        lane_t *ln = lane_find(&inst->lanes, target, param,
                               inst->lane_track, inst->lane_clip_slot);
        if (!ln) return;
        chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
        if (!pinfo) return;
        const int stepped = (pinfo->type == KNOB_TYPE_INT ||
                             pinfo->type == KNOB_TYPE_ENUM);
        float v = 0.0f;
        if (!lane_eval(ln, phase, lo, len, stepped, &v))
            return;
        inst->lanes_probe_value = v;
        inst->lanes_probe_stepped = stepped;
        inst->lanes_probe_have = 1;
        for (int i = 0; i < ln->n; i++) {
            if (fabs(ln->pts[i].phase - phase) < LANE_MIN_POINT_BEATS) {
                inst->lanes_probe_exact = 1;
                break;
            }
        }
        return;
    }

    if (strcmp(sub, "plock") == 0) {
        /* EVERY REFUSAL HERE HAS A NAME NOW.
         *
         * There were four silent returns and all of them left `plocked` at 0,
         * which is one bit for four unrelated causes. The one that actually
         * bit: a p-lock naming a parameter the module does not have creates
         * nothing and says nothing -- typing `roomsize` for freeverb's
         * `room_size` produced no lane and no complaint, which is
         * indistinguishable from the feature being broken.
         *
         * Same argument as lanes:plock_reason on the host side, which names
         * the refusals of the step->phase translation. This names the
         * refusals of the WRITE. */
        inst->lanes_last_plocked = 0;
        inst->lanes_plock_refusal = LANE_PLOCK_BAD_REQUEST;
        if (!val) return;
        char target[16] = {0}, param[32] = {0};
        double phase = 0.0;
        int consumed = 0;
        /* %n after the three fixed fields, so whatever remains is the VALUE
         * verbatim -- an enum option can contain spaces, and re-parsing it
         * with %s would silently keep only the first word. */
        /* "<target> <param> <phase> [<span>] <value>".
         *
         * The SPAN is optional and sits before the value, because the value is
         * whatever remains VERBATIM (an enum option can contain spaces, so
         * re-parsing it with %s would keep only the first word). Told apart
         * from a value by requiring BOTH a number there AND something after
         * it: "synth cutoff 1.0 70" parses as a span of 70 with nothing
         * following, which is how the four-field form identifies itself. */
        double span = 0.0;
        int consumed5 = 0;
        if (sscanf(val, "%15s %31s %lf %lf %n", target, param, &phase, &span,
                   &consumed5) == 4 && consumed5 > 0 && val[consumed5]) {
            consumed = consumed5;
        } else {
            span = 0.0;
            if (sscanf(val, "%15s %31s %lf %n", target, param, &phase, &consumed) < 3)
                return;
            if (consumed <= 0 || !val[consumed]) return;
        }
        if (!isfinite(span) || span < 0.0) span = 0.0;
        const char *value_str = val + consumed;
        if (!isfinite(phase) || phase < 0.0) return;
        inst->lanes_plock_refusal = LANE_PLOCK_NO_CLIP;
        if (inst->lane_track < 0 || inst->lane_clip_slot < 0) return;

        inst->lanes_plock_refusal = LANE_PLOCK_UNKNOWN_PARAM;
        chain_param_info_t *pinfo = find_param_by_key(inst, target, param);
        if (!pinfo) return;
        const float v = dsp_value_to_float(value_str, pinfo, pinfo->default_val);

        lane_fingerprint_t fp;
        lane_current_fingerprint(inst, &fp);
        inst->lanes_plock_refusal = LANE_PLOCK_STORE_FULL;
        lane_t *ln = lane_alloc(&inst->lanes, target, param,
                                inst->lane_track, inst->lane_clip_slot, &fp);
        if (!ln) return;
        /* A FULL LANE REFUSES A LOCK RATHER THAN MOVING SOMEBODY ELSE'S.
         *
         * lane_write's overflow rule takes the NEAREST point and relocates it
         * -- "degrade resolution rather than drop the gesture", which is right
         * for a recorded sweep, where losing a breakpoint mid-curve is a hole
         * the user cannot see or fix. It is wrong for a discrete edit:
         * measured, a lock written into a 64-point lane silently moved the
         * lock at phase 100.0 to phase 1.0 and reported success, mark and all.
         *
         * Refused with the name that already exists, so the UI can say why.
         * Only when the point would be NEW -- a lock replacing one already on
         * that step is not an overflow. */
        if (ln->n >= LANE_POINTS_MAX) {
            int on_step = 0;
            for (int i = 0; i < ln->n; i++)
                if (fabs(ln->pts[i].phase - phase) < LANE_MIN_POINT_BEATS) { on_step = 1; break; }
            if (!on_step) return;          /* refusal already named STORE_FULL */
        }
        lane_write_span(ln, phase, v, 1, span);
        inst->lanes_last_plocked = 1;
        inst->lanes_plock_refusal = LANE_PLOCK_OK;
        return;
    }

    /* MOVE DOUBLED THE LOOP (Shift+Step 15), which its manual describes as
     * doubling "notes and automation". Every lane at the playing position
     * copies its points one loop-length later, so the second half plays what
     * the first half did.
     *
     * Fired by the shim the moment it sees the gesture, NOT by watching the
     * clip's length change: the new length reaches Song.abl about 10 s later,
     * and a lane that waited would be silent over the new bars until then --
     * and indistinguishable from one that had simply failed. The copies land
     * in the second half, dormant until the window grows to include them,
     * which is the same rule that already governs a lengthened clip.
     *
     * Every lane for the CURRENT clip position, because the gesture acts on
     * the selected clip and a lane is bound to one. */
    if (strcmp(sub, "double") == 0) {
        /* Zeroed first, for the reason on copy_clip below. */
        inst->lanes_last_doubled = 0;
        if (!val || atoi(val) == 0) return;
        if (!(inst->clip_loop_len > 0.0) || !(inst->clip_loop_start >= 0.0)) return;
        int total = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            lane_t *ln = &inst->lanes.lanes[i];
            if (!ln->used || ln->stale || ln->orphaned) continue;
            if (ln->track != inst->lane_track || ln->slot != inst->lane_clip_slot)
                continue;
            total += lane_double(ln, inst->clip_loop_start, inst->clip_loop_len);
        }
        inst->lanes_last_doubled = total;
        return;
    }

    /* A CLIP WAS DUPLICATED: "<src_slot> <dst_slot>" on this track.
     *
     * Move's own Double Loop is documented as carrying automation, and a
     * duplicated CLIP is the same expectation -- a copy that arrives silent is
     * a copy of half the thing. The worker recognises the duplicate from the
     * file (see shadow_chain_mgmt.h) and the callback hands it here.
     *
     * The destination's lanes are REPLACED, not merged: a slot that was empty
     * a moment ago has no automation of its own worth preserving, and merging
     * two curves is not something the user could have asked for. The source is
     * untouched.
     *
     * The fingerprint is copied verbatim, which is exactly right: the
     * duplicate has the same notes, so the same fingerprint matches it. */
    if (strcmp(sub, "copy_clip") == 0) {
        /* ZEROED FIRST. Every `return` below is a refusal, and leaving the
         * previous call's count in place makes a refusal read as a success --
         * which is the exact ambiguity these counters exist to remove. */
        inst->lanes_last_copied = 0;
        int src = -1, dst = -1;
        if (!val || sscanf(val, "%d %d", &src, &dst) != 2) return;
        if (src < 0 || dst < 0 || src == dst) return;
        if (inst->lane_track < 0) return;
        const int track = inst->lane_track;
        int copied = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            const lane_t *from = &inst->lanes.lanes[i];
            if (!from->used || from->track != track || from->slot != src) continue;
            /* An empty or unidentified source carries nothing worth copying,
             * and copying the ABSENT fingerprint would plant a lane that can
             * never match anything. */
            if (from->n <= 0 || lane_fp_absent(&from->fp)) continue;
            lane_t *to = lane_alloc(&inst->lanes, from->target, from->param,
                                    track, dst, &from->fp);
            if (!to) break;          /* store full: as many as fit, in order */
            /* Points only -- never `driving`, `stale`, the punch pair or the
             * recording pass. Those describe THIS block on the source lane,
             * and lane_alloc has already zeroed them on a fresh slot. */
            to->n = 0;
            for (int k = 0; k < from->n; k++)
                lane_write(to, from->pts[k].phase, from->pts[k].value,
                           from->pts[k].hold);
            copied++;
        }
        inst->lanes_last_copied = copied;
        return;
    }

    if (strcmp(sub, "clear") == 0) {
        if (!val || atoi(val) == 0) return;
        lane_undo_take(inst);
        lane_release_all(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++)
            if (inst->lanes.lanes[i].used) n++;
        lane_store_reset(&inst->lanes);
        inst->lanes_last_cleared = n;
        return;
    }

    /* CLEAR ONE CLIP'S AUTOMATION. `lanes:clear` empties the whole SLOT --
     * every clip, every parameter -- which is the only granularity that
     * existed and is far blunter than the thing people want ("undo what I
     * just did to this clip"). The clip is the one this slot is currently
     * bound to; with nothing playing and nothing selected there is no clip to
     * name, so it refuses rather than guessing at one. */
    if (strcmp(sub, "clear_clip") == 0) {
        if (!val || atoi(val) == 0) return;
        inst->lanes_last_cleared = 0;
        if (inst->lane_clip_slot < 0) return;
        lane_undo_take(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            lane_t *ln = &inst->lanes.lanes[i];
            if (!lane_is_for_clip(ln, inst->lane_track, inst->lane_clip_slot))
                continue;
            if (ln->driving) lane_release_one(inst, ln);
            lane_clear_one(ln);
            n++;
        }
        inst->lanes_last_cleared = n;
        return;
    }

    /* CLEAR ONE PARAMETER'S LANE on the current clip: "<target> <param>".
     * The finest grain, and the one that matches how a mistake is made --
     * one knob, one clip. */
    if (strcmp(sub, "clear_param") == 0) {
        char target[16] = {0}, param[32] = {0};
        inst->lanes_last_cleared = 0;
        if (!val || sscanf(val, "%15s %31s", target, param) != 2) return;
        if (inst->lane_clip_slot < 0) return;
        lane_undo_take(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            lane_t *ln = &inst->lanes.lanes[i];
            if (!lane_is_for_param(ln, inst->lane_track, inst->lane_clip_slot,
                                   target, param))
                continue;
            if (ln->driving) lane_release_one(inst, ln);
            lane_clear_one(ln);
            n++;
        }
        inst->lanes_last_cleared = n;
        return;
    }

    /* CLEAR ONE COMPONENT'S AUTOMATION on the current clip: "<target>".
     *
     * The grain the MODULE PAGE offers, and the reason it exists there: you
     * record automation by turning a knob on a component's own pages, so
     * "clear what I just did to this module" belongs beside those knobs
     * rather than two menus away under the slot. Spans every parameter of
     * that component and no others. */
    if (strcmp(sub, "clear_target") == 0) {
        char target[16] = {0};
        inst->lanes_last_cleared = 0;
        if (!val || sscanf(val, "%15s", target) != 1) return;
        if (inst->lane_clip_slot < 0) return;
        lane_undo_take(inst);
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            lane_t *ln = &inst->lanes.lanes[i];
            if (!lane_is_for_clip(ln, inst->lane_track, inst->lane_clip_slot))
                continue;
            if (strcmp(ln->target, target) != 0) continue;
            if (ln->driving) lane_release_one(inst, ln);
            lane_clear_one(ln);
            n++;
        }
        inst->lanes_last_cleared = n;
        return;
    }

    /* UNDO, which is also REDO -- the buffer is swapped, not copied back.
     * Every override is released first: the lanes about to be swapped out are
     * holding them, and the set swapped in must re-establish its own. */
    if (strcmp(sub, "undo") == 0) {
        if (!val || atoi(val) == 0) return;
        if (!inst->lanes_undo_valid) { inst->lanes_last_undone = 0; return; }
        lane_release_all(inst);
        lane_store_swap(&inst->lanes, &inst->lanes_undo);
        inst->lanes_last_undone = 1;
        return;
    }
}

/* Bytes written, or -1 for "not a lane key we serve" -- which the UI reads as
 * a FAILED read rather than as an empty answer. The dispatch swallows the
 * whole "lanes:" prefix, so an unknown subkey has nothing left to fall through
 * to and must say so instead of answering "" and being believed. */
int lane_param_get(chain_instance_t *inst, const char *sub,
                   char *buf, int buf_len) {
    if (!inst || !sub || !buf || buf_len <= 0) return -1;

    /* 0 bytes means this slot has no automation; -1 means the host's buffer
     * was too small, which the UI must not mistake for empty or it truncates
     * a good lanes_<i>.json with half a document. */
    if (strcmp(sub, "state") == 0) return lane_serve_state(inst, buf, buf_len);

    /* WHICH CLIP this slot is bound to, as "<track> <slot>" 0-based, or empty
     * when it is bound to none. The UI needs it to NAME what a clip-scoped
     * action will act on: "Clear Clip" is a promise about a clip the user
     * cannot see from the row, and an empty answer is what lets the row say
     * so instead of clearing something unexpected. */
    if (strcmp(sub, "clip") == 0) {
        if (inst->lane_track < 0 || inst->lane_clip_slot < 0)
            return snprintf(buf, buf_len, "%s", "");
        return snprintf(buf, buf_len, "%d %d",
                        inst->lane_track, inst->lane_clip_slot);
    }

    /* The answer to the last `lanes:probe`: "<value> <exact>", or empty for
     * "this lane has nothing to say at that phase" -- which is NOT the value
     * 0.0, the same distinction lane_eval's return carries and the same one
     * the UI must not collapse, or every unautomated knob would read 0 while
     * a step is held. */
    if (strcmp(sub, "probe") == 0) {
        if (!inst->lanes_probe_have) return snprintf(buf, buf_len, "%s", "");
        if (inst->lanes_probe_stepped)
            return snprintf(buf, buf_len, "%d %d", (int)inst->lanes_probe_value,
                            inst->lanes_probe_exact);
        return snprintf(buf, buf_len, "%.6f %d", inst->lanes_probe_value,
                        inst->lanes_probe_exact);
    }

    if (strcmp(sub, "plock_refused") == 0) {
        static const char *names[] = {
            "ok", "bad_request", "no_clip", "unknown_param", "store_full"
        };
        int r = inst->lanes_plock_refusal;
        if (r < 0 || r > LANE_PLOCK_STORE_FULL) r = LANE_PLOCK_BAD_REQUEST;
        return snprintf(buf, buf_len, "%d %s", r, names[r]);
    }

    if (strcmp(sub, "undone") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_last_undone);

    if (strcmp(sub, "undoable") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_undo_valid ? 1 : 0);

    if (strcmp(sub, "cleared") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_last_cleared);
    if (strcmp(sub, "plocked") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_last_plocked);
    if (strcmp(sub, "doubled") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_last_doubled);
    if (strcmp(sub, "copied") == 0)
        return snprintf(buf, buf_len, "%d", inst->lanes_last_copied);

    /* WHY a recording was refused. 0 is UNKNOWN -- the shim could not say
     * where in the clip we are -- and not phase zero, which is what lets the
     * UI say "clip phase unknown" instead of leaving the user to guess why a
     * knob turn recorded nothing. */
    if (strcmp(sub, "phase_valid") == 0)
        return snprintf(buf, buf_len, "%d", inst->clip_phase_valid ? 1 : 0);

    /* Readable as well as writable: the arm comes from Move's Record LED
     * through the shim, so the UI has no other way to know it. */
    /* HOW MANY LANES ARE DRIVING A PARAMETER RIGHT NOW.
     *
     * The playback half of "you cannot tell what is happening". A lane's value
     * lands on the module through an override, so on the knob grid it shows as
     * the mod dot riding the arc -- but a module that draws its OWN screen has
     * no such mark, and automation running was indistinguishable from nothing
     * running. This is the slot-altitude form of the same fact the per-key
     * `:modulated` already answers.
     *
     * `driving` is the flag lane_tick sets when it emits an override and
     * lane_release_one clears, so this needs no second notion of "active" --
     * it counts the lanes that ARE speaking, not the ones that exist. */
    if (strcmp(sub, "driving") == 0) {
        int n = 0;
        for (int i = 0; i < LANE_MAX; i++)
            if (inst->lanes.lanes[i].used && inst->lanes.lanes[i].driving) n++;
        return snprintf(buf, buf_len, "%d", n);
    }

    /* WOULD THIS WRITE BE RECORDED? Read by the host to refuse turning a
     * recording pass into p-locks -- see lane_is_recording. It IS the
     * predicate, not a restatement of it. */
    if (strcmp(sub, "recording") == 0)
        return snprintf(buf, buf_len, "%d", lane_is_recording(inst) ? 1 : 0);

    if (strcmp(sub, "armed") == 0)
        return snprintf(buf, buf_len, "%d", inst->lane_armed ? 1 : 0);

    return -1;
}
