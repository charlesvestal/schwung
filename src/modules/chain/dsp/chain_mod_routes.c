/*
 * Signal Chain — mod ROUTES: the eight per-slot modulation sources and the
 * "mod<N>:" / legacy "lfo<N>:" parameter ladders that configure them.
 *
 * Sibling of chain_mod.c, and the distinction matters:
 *
 *   chain_mod.c        the BUS. Where a contribution lands: the non-destructive
 *                      base/effective overlay, the range scaling, the summing
 *                      of up to 8 sources per target. It knows nothing about
 *                      where a signal came from, which is what let MIDI sources
 *                      be added without touching it at all.
 *   chain_mod_routes.c the PRODUCERS. What a route IS — its source type, its
 *                      depth and polarity, its destination — and how a param
 *                      key reaches it.
 *
 * Split out of chain_host.c rather than added to it: that file is pinned below
 * 2900 lines by tests/host/test_chain_host_file_split.sh, which exists because
 * it had re-accreted to 6,450 once already. A cluster this size with its own
 * vocabulary is exactly what that pin is asking for.
 *
 * REALTIME: every function here is reached from v2_set_param / v2_get_param,
 * which are the SPI callback. No allocation, no I/O, no logging.
 */

#include "chain_internal.h"

/*
 * Resolve a "mod<N>:" or legacy "lfo<N>:" prefix to a 0-based route index.
 *
 * The parsing itself lives in src/host/mod_route_key.h, beside bus_route.h and
 * send_fx_key.h, so tests/host can run it natively -- and so the cap arrives as
 * an ARGUMENT rather than as a copy of MOD_ROUTE_COUNT the header would have to
 * keep in step. This wrapper exists only to bind that argument once.
 */
int chain_mod_route_index(const char *key, const char **out_rest) {
    return mod_route_parse_key(key, MOD_ROUTE_COUNT, out_rest);
}

/*
 * Configure one mod route. Returns 1 if `key` named one and was handled, 0 if
 * the key is not ours, so v2_set_param falls through to its next branch exactly
 * as the inline ladder used to.
 */
int chain_mod_route_set_param(chain_instance_t *inst, const char *key, const char *val) {
    const char *subkey = NULL;
    int route_idx = chain_mod_route_index(key, &subkey);
    if (route_idx < 0) return 0;
    if (!inst || !val) return 1;   /* ours, and nothing sensible to do with it */

    lfo_state_t *lfo = &inst->mod_routes[route_idx];

    /*
     * "mod%d" for ALL EIGHT, including the two the legacy keys also reach.
     * This string is what chain_mod_clear_source matches on, so it names the
     * ROUTE rather than the spelling that addressed it. If routes 1 and 2 kept
     * emitting as "lfo1"/"lfo2" while the clear paths said "mod1"/"mod2", a
     * patch load would clear a source that does not exist and leave the real
     * one running — a stuck modulation with nothing in the UI to turn it off.
     */
    char source_id[8];
    snprintf(source_id, sizeof(source_id), "mod%d", route_idx + 1);

    if (strcmp(subkey, "enabled") == 0) {
        lfo->enabled = atoi(val);
        if (!lfo->enabled) {
            lfo->active = 0;
            chain_mod_clear_source(inst, source_id);
        } else {
            /* Set sensible defaults if this is a fresh LFO (rate_hz still 0) */
            if (lfo->rate_hz < 0.1f && !lfo->sync) {
                lfo->rate_hz = 1.0f;
            }
            /*
             * Full depth, not half. A route you have just switched on should
             * DO something — at 50% the effect was there but easy to miss, and
             * the row-wide waveform drawn on the route's page reads as a
             * half-height wave for no reason the user chose.
             *
             * The guard is what makes this safe: it fires only when depth is
             * exactly 0 AND no target has been picked yet, i.e. a genuinely
             * fresh route. Anything already configured, or restored from a
             * saved slot, sets depth explicitly and is untouched.
             */
            if (lfo->depth == 0.0f && !lfo->target[0] && !lfo->param[0]) {
                lfo->depth = 1.0f;
            }
            lfo->active = (lfo->target[0] && lfo->param[0]);
        }
    } else if (strcmp(subkey, "shape") == 0) {
        lfo->shape = atoi(val);
        if (lfo->shape < 0) lfo->shape = 0;
        if (lfo->shape >= LFO_NUM_SHAPES) lfo->shape = LFO_NUM_SHAPES - 1;
    } else if (strcmp(subkey, "rate_hz") == 0) {
        lfo->rate_hz = strtof(val, NULL);
        if (lfo->rate_hz < 0.1f) lfo->rate_hz = 0.1f;
        if (lfo->rate_hz > 20.0f) lfo->rate_hz = 20.0f;
    } else if (strcmp(subkey, "rate_div") == 0) {
        lfo->rate_div = atoi(val);
        if (lfo->rate_div < 0) lfo->rate_div = 0;
        if (lfo->rate_div >= LFO_NUM_DIVISIONS) lfo->rate_div = LFO_NUM_DIVISIONS - 1;
    } else if (strcmp(subkey, "sync") == 0) {
        lfo->sync = atoi(val);
        /* Default to 1/1 (index 15) if rate_div is still at 0 (16bar) */
        if (lfo->sync && lfo->rate_div == 0) lfo->rate_div = 15;
    } else if (strcmp(subkey, "depth") == 0) {
        lfo->depth = strtof(val, NULL);
        if (lfo->depth < -1.0f) lfo->depth = -1.0f;
        if (lfo->depth > 1.0f) lfo->depth = 1.0f;
    } else if (strcmp(subkey, "polarity") == 0) {
        lfo->bipolar = atoi(val) ? 1 : 0;
    } else if (strcmp(subkey, "phase_offset") == 0) {
        lfo->phase_offset = strtof(val, NULL);
        if (lfo->phase_offset < 0.0f) lfo->phase_offset = 0.0f;
        if (lfo->phase_offset > 1.0f) lfo->phase_offset = 1.0f;
    } else if (strcmp(subkey, "target") == 0) {
        /* Clear old modulation source before changing target */
        if (lfo->target[0]) {
            chain_mod_clear_source(inst, source_id);
        }
        strncpy(lfo->target, val, sizeof(lfo->target) - 1);
        lfo->target[sizeof(lfo->target) - 1] = '\0';
        lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
        inst->mod_route_base_valid[route_idx] = 0;  /* Re-snapshot base */
    } else if (strcmp(subkey, "target_param") == 0) {
        /* Clear old modulation source before changing param */
        if (lfo->param[0]) {
            chain_mod_clear_source(inst, source_id);
        }
        strncpy(lfo->param, val, sizeof(lfo->param) - 1);
        lfo->param[sizeof(lfo->param) - 1] = '\0';
        lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
        inst->mod_route_base_valid[route_idx] = 0;  /* Re-snapshot base */
    } else if (strcmp(subkey, "retrigger") == 0) {
        lfo->retrigger = atoi(val);
        lfo->held_count = 0;  /* Reset on toggle */
    } else if (strcmp(subkey, "src") == 0) {
        /*
         * Accepts the WIRE NAME and a raw enum index. The knob grid writes an
         * index, because that is what its `enum` type is; the patch file writes
         * a name, because a stored index would silently mean a different source
         * if mod_src_names were ever reordered. One branch rather than two call
         * sites that can disagree.
         */
        int t = (val[0] >= '0' && val[0] <= '9') ? atoi(val) : mod_src_from_name(val);
        if (t < 0 || t >= MOD_SRC_COUNT) t = MOD_SRC_LFO;
        if (t != lfo->src) {
            lfo->src = t;
            /* Seed the slew from the NEW source's value on the next block
             * rather than gliding there from the old source's — a glide between
             * two unrelated controls reads as a fault in the synth rather than
             * as a transition in the matrix. */
            lfo->slew_primed = 0;
        }
        lfo->active = (lfo->enabled && lfo->target[0] && lfo->param[0]);
    } else if (strcmp(subkey, "cc_num") == 0) {
        lfo->cc_num = atoi(val);
        if (lfo->cc_num < 0) lfo->cc_num = 0;
        if (lfo->cc_num > 127) lfo->cc_num = 127;
    } else if (strcmp(subkey, "slew") == 0) {
        lfo->slew = strtof(val, NULL);
        if (lfo->slew < 0.0f) lfo->slew = 0.0f;
        /* Capped below 1: mod_src_slew treats 1.0 as "never arrive", which is a
         * route that is on, aimed, and permanently stuck at whatever it was
         * seeded with. Reachable from a knob, so clamped here rather than
         * trusted. */
        if (lfo->slew > 0.99f) lfo->slew = 0.99f;
    }

    inst->dirty = 1;
    return 1;
}

/*
 * Read one mod route field. Returns 1 if `key` named one, with *out_len set to
 * what v2_get_param should return (which may be -1 for an unknown subkey of a
 * real route — ours, but unanswerable). Returns 0 if the key is not ours.
 */
int chain_mod_route_get_param(chain_instance_t *inst, const char *key,
                              char *buf, int buf_len, int *out_len) {
    const char *subkey = NULL;
    int route_idx = chain_mod_route_index(key, &subkey);
    if (route_idx < 0) return 0;
    if (!inst || !buf || buf_len < 2) { if (out_len) *out_len = -1; return 1; }

    lfo_state_t *lfo = &inst->mod_routes[route_idx];
    int n = -1;

    if (strcmp(subkey, "enabled") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->enabled);
    else if (strcmp(subkey, "active") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->active);
    else if (strcmp(subkey, "shape") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->shape);
    else if (strcmp(subkey, "shape_name") == 0)
        n = snprintf(buf, buf_len, "%s",
                     (lfo->shape >= 0 && lfo->shape < LFO_NUM_SHAPES)
                     ? lfo_shape_names[lfo->shape] : "sine");
    else if (strcmp(subkey, "rate_hz") == 0)
        n = snprintf(buf, buf_len, "%.1f", lfo->rate_hz);
    else if (strcmp(subkey, "rate_div") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->rate_div);
    else if (strcmp(subkey, "rate_div_label") == 0)
        n = snprintf(buf, buf_len, "%s",
                     (lfo->rate_div >= 0 && lfo->rate_div < LFO_NUM_DIVISIONS)
                     ? lfo_divisions[lfo->rate_div].label : "1/4");
    else if (strcmp(subkey, "sync") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->sync);
    else if (strcmp(subkey, "depth") == 0)
        n = snprintf(buf, buf_len, "%.2f", lfo->depth);
    else if (strcmp(subkey, "polarity") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->bipolar);
    else if (strcmp(subkey, "phase_offset") == 0)
        n = snprintf(buf, buf_len, "%.2f", lfo->phase_offset);
    else if (strcmp(subkey, "target") == 0)
        n = snprintf(buf, buf_len, "%s", lfo->target);
    else if (strcmp(subkey, "target_param") == 0)
        n = snprintf(buf, buf_len, "%s", lfo->param);
    else if (strcmp(subkey, "retrigger") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->retrigger);
    /* The wire NAME, not the index — see the set side. A stored index would
     * mean a different source the moment mod_src_names is reordered. */
    else if (strcmp(subkey, "src") == 0)
        n = snprintf(buf, buf_len, "%s", mod_src_name(lfo->src));
    /* The INDEX, for the knob grid's enum cell, which counts rather than reads
     * words. Both spellings exist because both callers are right. */
    else if (strcmp(subkey, "src_index") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->src);
    else if (strcmp(subkey, "cc_num") == 0)
        n = snprintf(buf, buf_len, "%d", lfo->cc_num);
    else if (strcmp(subkey, "slew") == 0)
        n = snprintf(buf, buf_len, "%.2f", lfo->slew);

    if (out_len) *out_len = n;
    return 1;
}
