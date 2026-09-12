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
    if (!inst->clip_phase_valid || !(inst->clip_loop_len > 0.0)) {
        lane_release_all(inst);
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
            continue;
        }

        /* An unarmed knob turn punches through until the loop comes round --
         * otherwise, under an absolute lane, turning a knob does nothing
         * audible and reads as a broken encoder. Set in Task 5. */
        if (ln->punch_until_wrap) {
            if (inst->clip_phase_beats < ln->punch_phase) ln->punch_until_wrap = 0;
            else continue;
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
        if (!lane_eval(ln, inst->clip_phase_beats, inst->clip_loop_len,
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

/* The clip's fingerprint as it is RIGHT NOW. Task 6 is what fills it with real
 * note data; until then clip_fp_valid is 0 and a lane records with an all-zero
 * fingerprint, which nothing compares against -- an unknown clip is not a
 * mismatched one, and a guessed fingerprint would make every lane stale the
 * moment matching arrives. */
void lane_current_fingerprint(chain_instance_t *inst, lane_fingerprint_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (inst && inst->clip_fp_valid) *out = inst->clip_fp;
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
    if (inst->lane_armed && inst->clip_phase_valid && inst->clip_loop_len > 0.0) {
        lane_fingerprint_t fp;
        lane_current_fingerprint(inst, &fp);
        lane_t *ln = lane_alloc(&inst->lanes, target, param,
                                inst->lane_track, inst->lane_clip_slot, &fp);
        /* Store full, or a target/param too long for lane_t's fields, which
         * lane_alloc REFUSES rather than truncating -- a truncated key would
         * name a lane the user can neither see nor clear. Nothing is recorded
         * and nothing pretends to have been. */
        if (!ln) return;
        lane_write(ln, inst->clip_phase_beats, v);
        /* An armed turn IS this lane, so it cancels any punch a previous
         * unarmed turn left open; otherwise the point just recorded would sit
         * silent until the loop came round. */
        ln->punch_until_wrap = 0;
        return;
    }

    /* Unarmed (or phaseless) under an existing lane: hand the parameter to the
     * knob until the loop comes round. Without this, an absolute lane rewrites
     * the same target every block and the encoder is inaudible -- which reads
     * as broken hardware, not as automation. */
    lane_t *ln = lane_find(&inst->lanes, target, param);
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
