/*
 * chain_bus.c — the "bus<N>:" parameter surface, the voice map, the patch
 * apply, and the worker's reconcile step.
 *
 * A sibling of chain_host.c rather than more of it, for the same reason
 * chain_patch.c and chain_reorder.c are.
 *
 * ================= WHICH THREAD OWNS WHAT ==================================
 *
 * TWO THREADS touch slot_bus_t and the split is the whole design:
 *
 *   RT (the SPI callback: set_param, get_param, render_block, and everything
 *   chain_patch.c calls) owns the REQUEST — fx_request[], fx_state_request[],
 *   fx_bypassed[], voice_ids[], send_level[], in_use, name. It never allocates,
 *   never opens a file and never dlopens.
 *
 *   The WORKER (chain_bus_worker_fn, SCHED_OTHER on cores 0-2) owns the
 *   REALISATION — buf, fx_handles[], fx_plugins_v2[], fx_instances[],
 *   fx_params[], fx_ui_hierarchy[], fx_count, current_fx_modules[]. Every
 *   dlopen, create_instance and multi-megabyte calloc in this feature happens
 *   there and nowhere else.
 *
 * Because get_param answers from the REQUEST side, the UI never has to read a
 * field the worker is writing, and the "what is loaded" answer is positional
 * and immediate rather than lagging a load.
 *
 * The two are joined by exactly two gates: `buf` (RELEASE/ACQUIRE, published by
 * the worker) and `fx_ready` (likewise, covering everything in the second list
 * above). See slot_bus_t's own comments — the second gate exists because the
 * first orders `buf` and nothing else.
 *
 * set_param and render_block are the SAME thread, which is what makes the RT
 * side's plain `fx_ready = 0` sufficient: once set_param has cleared it, no
 * later render can enter the bus's insert loop, and no earlier one is still
 * inside it.
 */
#include "chain_internal.h"
#include "host/bus_voice_apply.h"

/* ============================================================================
 * Small shared helpers
 * ============================================================================ */

/* Path-traversal guard, applied on the RT side when a request is STORED, so
 * the worker only ever dlopens a name that was already checked. (chain_host.c
 * has its own file-static copy for the main chain; this one exists so the check
 * happens at the point the user's string arrives, where it can be refused.) */
static int bus_valid_module_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return 0;
    return 1;
}

static int bus_clamp_send(int v) {
    if (v < 0) return 0;
    if (v > BUS_MIX_SEND_LEVEL_MAX) return BUS_MIX_SEND_LEVEL_MAX;
    return v;
}

/* Parse a trailing 1-based index off a fixed prefix: "send2" -> 2. Returns -1
 * on no match. Deliberately strict (no leading zeros, digits only, nothing
 * after), matching chain_key_index.h — a key we do not recognise must fall
 * through, never land on index 0. */
static int bus_suffix_index(const char *sub, const char *prefix, int max)
{
    size_t pl = strlen(prefix);
    if (strncmp(sub, prefix, pl) != 0) return -1;
    const char *p = sub + pl;
    if (*p < '1' || *p > '9') return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (n < 100000) n = n * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0') return -1;
    return (n >= 1 && n <= max) ? n : -1;
}

/*
 * Hand a bus to the worker.
 *
 * Clearing fx_ready FIRST is what makes the handover safe: the render path
 * stops entering this bus's insert chain from the very next frame, and this is
 * the same thread the render runs on, so there is no in-flight reader to wait
 * for. The seq bump is what stops the worker publishing a stale "done" — see
 * slot_bus_t::fx_req_seq.
 *
 * RT-safe: two stores and a sem_post.
 */
static void bus_request_work(chain_instance_t *inst, int b)
{
    slot_bus_t *bus = &inst->buses[b];
    __atomic_store_n(&bus->fx_ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&bus->fx_req_seq, bus->fx_req_seq + 1, __ATOMIC_RELEASE);
    /*
     * A slot with no buses must still cost NO THREAD. chain_bus_post_work
     * starts the worker lazily, so posting from here unconditionally would
     * spawn one on every patch load — chain_bus_apply_patch resets all
     * SLOT_BUSES positions whether or not the file mentioned any. Flag the
     * request and leave: a bus that has never existed has nothing to
     * reconcile, and the flag is still set when some other bus does start the
     * worker.
     */
    if (!inst->bus_worker_started && !bus->in_use) {
        __atomic_store_n(&inst->bus_alloc_pending[b], 1, __ATOMIC_RELEASE);
        return;
    }
    chain_bus_post_work(inst, b);
}

/* ============================================================================
 * Voice map
 * ============================================================================ */

void chain_bus_rebuild_voice_map(chain_instance_t *inst)
{
    if (!inst) return;
    /* Reset first, then apply every bus: bus_voice_apply only ever WRITES, so
     * the result must not depend on the order the buses are applied in. */
    chain_reset_voice_bus(inst);

    /* The module's declared list, as the pointer array bus_voice_index takes.
     * A stack array of 32 pointers on the SPI callback — no allocation. */
    const char *ids[SPLIT_VOICES_MAX];
    int n_ids = inst->synth_split_voice_count;
    if (n_ids > SPLIT_VOICES_MAX) n_ids = SPLIT_VOICES_MAX;
    if (n_ids < 0) n_ids = 0;
    for (int i = 0; i < n_ids; i++) ids[i] = inst->synth_split_voice_ids[i];

    for (int b = 0; b < SLOT_BUSES; b++) {
        slot_bus_t *bus = &inst->buses[b];
        int n = bus->voice_id_count;
        if (n > SPLIT_VOICES_MAX) n = SPLIT_VOICES_MAX;
        if (n <= 0) { bus->orphan_count = 0; continue; }
        const char *stored[SPLIT_VOICES_MAX];
        for (int i = 0; i < n; i++) stored[i] = bus->voice_ids[i];
        bus->orphan_count = bus_voice_apply(ids, n_ids, stored, n, b,
                                            inst->voice_bus, SPLIT_VOICES_MAX);
    }
}

/* Replace a bus's stored voice id list from a comma-separated string.
 * Unknown ids are KEPT — resolution happens in the rebuild, and an id that does
 * not resolve today may resolve after the module loads. */
static void bus_set_voice_ids(slot_bus_t *bus, const char *csv)
{
    bus->voice_id_count = 0;
    memset(bus->voice_ids, 0, sizeof(bus->voice_ids));
    if (!csv) return;
    const char *p = csv;
    while (*p && bus->voice_id_count < SPLIT_VOICES_MAX) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        while (len > 0 && start[len - 1] == ' ') len--;
        if (len <= 0) continue;
        if (len > SPLIT_VOICE_ID_LEN - 1) len = SPLIT_VOICE_ID_LEN - 1;
        memcpy(bus->voice_ids[bus->voice_id_count], start, len);
        bus->voice_ids[bus->voice_id_count][len] = '\0';
        bus->voice_id_count++;
    }
}

/* ============================================================================
 * Teardown (RT side)
 * ============================================================================ */

/*
 * Return a bus to its resting state.
 *
 * The BUFFER is unpublished HERE, on the RT thread, and only then handed to the
 * worker to free. A worker that freed a pointer the render path could still
 * load would be a use-after-free on the audio thread; storing NULL over it from
 * the thread that reads it makes the handover ordered by program order alone.
 */
static void bus_reset(chain_instance_t *inst, int b)
{
    slot_bus_t *bus = &inst->buses[b];
    bus->in_use = 0;
    bus->name[0] = '\0';
    bus->voice_id_count = 0;
    bus->orphan_count = 0;
    memset(bus->voice_ids, 0, sizeof(bus->voice_ids));
    for (int i = 0; i < BUS_MIX_SENDS; i++) bus->send_level[i] = 0;
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        bus->fx_request[i][0] = '\0';
        bus->fx_state_request[i][0] = '\0';
        __atomic_store_n(&bus->fx_state_pending[i], 0, __ATOMIC_RELAXED);
        bus->fx_bypassed[i] = 0;
    }
    /* Only one retirement can be in flight; if the worker has not yet taken the
     * previous one, keep it (the newer buf is NULL anyway) rather than leaking. */
    int16_t *old = __atomic_exchange_n(&bus->buf, NULL, __ATOMIC_ACQ_REL);
    if (old) {
        int16_t *prev = __atomic_exchange_n(&bus->buf_retired, old, __ATOMIC_ACQ_REL);
        /* prev is non-NULL only if two resets raced the worker, which needs two
         * deletes inside one worker wake. Freeing it here is RT-unsafe but the
         * alternative is a leak; it cannot happen while the render path is the
         * only other reader, because `buf` was already NULL for prev. */
        if (prev) free(prev);
    }
    bus_request_work(inst, b);
}

void chain_bus_clear_all(chain_instance_t *inst)
{
    if (!inst) return;
    for (int b = 0; b < SLOT_BUSES; b++) bus_reset(inst, b);
    for (int i = 0; i < BUS_MIX_SENDS; i++) inst->main_send_level[i] = 0;
    chain_bus_rebuild_voice_map(inst);
}

/* ============================================================================
 * RT: set_param
 * ============================================================================ */

int chain_bus_set_param(chain_instance_t *inst, int b, const char *sub, const char *val)
{
    if (!inst || b < 0 || b >= SLOT_BUSES || !sub) return -1;
    slot_bus_t *bus = &inst->buses[b];
    const char *v = val ? val : "";

    if (strcmp(sub, "create") == 0) {
        /* Idempotent: re-creating a live bus must not retire its buffer and
         * silence it for a frame. */
        if (!bus->in_use) {
            bus->in_use = 1;
            if (!bus->name[0])
                snprintf(bus->name, sizeof(bus->name), "Bus %d", b + 1);
            /* The allocation is a REQUEST — nothing is allocated on this
             * thread. chain_bus_request_alloc also starts the worker lazily,
             * so a slot with no buses costs no thread. */
            chain_bus_request_alloc(inst, b);
        }
        return 0;
    }
    if (strcmp(sub, "delete") == 0) {
        bus_reset(inst, b);
        chain_bus_rebuild_voice_map(inst);
        inst->dirty = 1;
        return 0;
    }
    if (strcmp(sub, "name") == 0) {
        strncpy(bus->name, v, MAX_NAME_LEN - 1);
        bus->name[MAX_NAME_LEN - 1] = '\0';
        inst->dirty = 1;
        return 0;
    }
    if (strcmp(sub, "voices") == 0) {
        bus_set_voice_ids(bus, v);
        chain_bus_rebuild_voice_map(inst);
        inst->dirty = 1;
        return 0;
    }
    {
        int s = bus_suffix_index(sub, "send", BUS_MIX_SENDS);
        if (s > 0) {
            /* THIS is the write that makes the whole feature audible:
             * chain_drain_sends reads send_level every frame and everything
             * downstream of it — the shim's two global send buses, their
             * inserts, their returns and their stems — has been inert without
             * it. */
            bus->send_level[s - 1] = bus_clamp_send(atoi(v));
            inst->dirty = 1;
            return 0;
        }
    }

    /* fx<K>:... */
    {
        const char *fxsub = NULL;
        int k = chain_fx_index_from_key(sub, "fx", MAX_AUDIO_FX, &fxsub);
        if (k >= 0 && fxsub) {
            if (strcmp(fxsub, "module") == 0) {
                const char *want = v;
                if (want[0] && strcmp(want, "none") != 0 && !bus_valid_module_name(want))
                    return 0;   /* refused here, so the worker never sees it */
                if (!want[0] || strcmp(want, "none") == 0) bus->fx_request[k][0] = '\0';
                else {
                    strncpy(bus->fx_request[k], want, MAX_NAME_LEN - 1);
                    bus->fx_request[k][MAX_NAME_LEN - 1] = '\0';
                }
                bus->fx_bypassed[k] = 0;
                bus_request_work(inst, b);
                inst->dirty = 1;
                return 0;
            }
            if (strcmp(fxsub, "bypassed") == 0) {
                /* RT-owned and read by the render path with a plain load, which
                 * is sound because the worker never writes it. No handover. */
                bus->fx_bypassed[k] = atoi(v) ? 1 : 0;
                inst->dirty = 1;
                return 0;
            }
            if (strcmp(fxsub, "state") == 0) {
                /* STAGED, not applied. The instance may not exist yet (a patch
                 * load writes the module and its state in the same breath), and
                 * applying it is the worker's job for the same reason creating
                 * the instance is. */
                strncpy(bus->fx_state_request[k], v, MAX_BUS_FX_STATE_LEN - 1);
                bus->fx_state_request[k][MAX_BUS_FX_STATE_LEN - 1] = '\0';
                __atomic_store_n(&bus->fx_state_pending[k], 1, __ATOMIC_RELEASE);
                bus_request_work(inst, b);
                inst->dirty = 1;
                return 0;
            }
            /* A live parameter edit goes straight to the plugin, and ONLY while
             * fx_ready says the instance pointer is stable and ours to read.
             * A write during a reconcile is dropped rather than queued: the UI
             * re-sends on the next detent, and a queue here would be a second
             * source of truth for a value the plugin already owns. */
            if (__atomic_load_n(&bus->fx_ready, __ATOMIC_ACQUIRE) &&
                bus->fx_plugins_v2[k] && bus->fx_instances[k] &&
                bus->fx_plugins_v2[k]->set_param) {
                bus->fx_plugins_v2[k]->set_param(bus->fx_instances[k], fxsub, v);
                inst->dirty = 1;
            }
            return 0;
        }
    }
    return -1;   /* not ours; the caller falls through */
}

int chain_bus_slot_set_param(chain_instance_t *inst, const char *sub, const char *val)
{
    if (!inst || !sub) return -1;
    int s = bus_suffix_index(sub, "main_send", BUS_MIX_SENDS);
    if (s > 0) {
        inst->main_send_level[s - 1] = bus_clamp_send(val ? atoi(val) : 0);
        inst->dirty = 1;
        return 0;
    }
    if (strcmp(sub, "clear") == 0) { chain_bus_clear_all(inst); return 0; }
    return -1;
}

/* ============================================================================
 * RT: get_param
 * ============================================================================ */

/* Escape a value into a JSON string body. Bus names come from the user's
 * keyboard, so a quote or backslash in one would otherwise produce a document
 * the UI cannot parse — and a name is exactly the field a person puts an
 * apostrophe in. */
static int bus_json_escape(char *out, int out_len, const char *in)
{
    int o = 0;
    for (const char *p = in; *p && o < out_len - 7; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += snprintf(out + o, out_len - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    if (o < out_len) out[o] = '\0';
    return o;
}

/*
 * "buses:config" — ONE GET returning every bus, positional and never compacted.
 *
 * Positional because an empty bus in the middle is a real state and compacting
 * it away renumbers everything behind it, which is the defect that lost the
 * Master FX chain. Opaque FX state is NOT included: it is per-position and
 * large, and the caller reads it as "bus<N>:fx<K>:state" exactly as it already
 * does for the main chain.
 */
static int bus_emit_config(chain_instance_t *inst, char *buf, int buf_len)
{
    int o = 0;
    char esc[MAX_NAME_LEN * 6 + 8];
    o += snprintf(buf + o, buf_len - o, "{\"buses\":[");
    for (int b = 0; b < SLOT_BUSES && o < buf_len - 256; b++) {
        slot_bus_t *bus = &inst->buses[b];
        if (b) o += snprintf(buf + o, buf_len - o, ",");
        bus_json_escape(esc, sizeof(esc), bus->name);
        o += snprintf(buf + o, buf_len - o,
                      "{\"present\":%d,\"name\":\"%s\",\"orphans\":%d,\"voices\":[",
                      bus->in_use ? 1 : 0, esc, bus->orphan_count);
        for (int i = 0; i < bus->voice_id_count && i < SPLIT_VOICES_MAX &&
                        o < buf_len - 128; i++) {
            bus_json_escape(esc, sizeof(esc), bus->voice_ids[i]);
            o += snprintf(buf + o, buf_len - o, "%s\"%s\"", i ? "," : "", esc);
        }
        o += snprintf(buf + o, buf_len - o, "],\"sends\":[");
        for (int i = 0; i < BUS_MIX_SENDS; i++)
            o += snprintf(buf + o, buf_len - o, "%s%d", i ? "," : "", bus->send_level[i]);
        o += snprintf(buf + o, buf_len - o, "],\"fx\":[");
        for (int i = 0; i < MAX_AUDIO_FX && o < buf_len - 160; i++) {
            bus_json_escape(esc, sizeof(esc), bus->fx_request[i]);
            o += snprintf(buf + o, buf_len - o,
                          "%s{\"module\":\"%s\",\"bypassed\":%d}",
                          i ? "," : "", esc, bus->fx_bypassed[i] ? 1 : 0);
        }
        o += snprintf(buf + o, buf_len - o, "]}");
    }
    o += snprintf(buf + o, buf_len - o, "],\"main_sends\":[");
    for (int i = 0; i < BUS_MIX_SENDS; i++)
        o += snprintf(buf + o, buf_len - o, "%s%d", i ? "," : "", inst->main_send_level[i]);
    o += snprintf(buf + o, buf_len - o, "]}");
    return o;
}

int chain_bus_get_param(chain_instance_t *inst, int b, const char *sub,
                        char *buf, int buf_len)
{
    if (!inst || b < 0 || b >= SLOT_BUSES || !sub || !buf || buf_len <= 0) return -1;
    slot_bus_t *bus = &inst->buses[b];

    if (strcmp(sub, "present") == 0)
        return snprintf(buf, buf_len, "%d", bus->in_use ? 1 : 0);
    if (strcmp(sub, "name") == 0)
        return snprintf(buf, buf_len, "%s", bus->name);
    /* The count, not the ids: a partial restore that reports nothing is
     * indistinguishable from a working one. The ids themselves are still in
     * "voices" below, because they are RETAINED rather than dropped. */
    if (strcmp(sub, "orphans") == 0)
        return snprintf(buf, buf_len, "%d", bus->orphan_count);
    if (strcmp(sub, "voices") == 0) {
        int o = 0;
        for (int i = 0; i < bus->voice_id_count && i < SPLIT_VOICES_MAX; i++) {
            if (o >= buf_len - 1) break;
            o += snprintf(buf + o, buf_len - o, "%s%s", i ? "," : "", bus->voice_ids[i]);
        }
        if (o == 0 && buf_len > 0) buf[0] = '\0';
        return o;
    }
    {
        int s = bus_suffix_index(sub, "send", BUS_MIX_SENDS);
        if (s > 0) return snprintf(buf, buf_len, "%d", bus->send_level[s - 1]);
    }
    {
        const char *fxsub = NULL;
        int k = chain_fx_index_from_key(sub, "fx", MAX_AUDIO_FX, &fxsub);
        if (k >= 0 && fxsub) {
            /* Answered from the REQUEST, not from current_fx_modules: the
             * request is RT-owned so it cannot tear under a read, and it is the
             * answer the user is owed — "what this position holds" must not
             * lag a load that is still on the worker's queue. */
            if (strcmp(fxsub, "module") == 0)
                return snprintf(buf, buf_len, "%s", bus->fx_request[k]);
            if (strcmp(fxsub, "bypassed") == 0)
                return snprintf(buf, buf_len, "%d", bus->fx_bypassed[k] ? 1 : 0);
            /* Everything below reads worker-owned memory. The ACQUIRE is the
             * gate; a read that arrives mid-reconcile answers -1, which the
             * param channel presents as "the read did not complete" — the
             * caller retries rather than caching a verdict. */
            if (!__atomic_load_n(&bus->fx_ready, __ATOMIC_ACQUIRE)) return -1;
            if (strcmp(fxsub, "ui_hierarchy") == 0) {
                if (bus->fx_ui_hierarchy[k] && bus->fx_ui_hierarchy[k][0]) {
                    int len = (int)strlen(bus->fx_ui_hierarchy[k]);
                    if (len < buf_len) { memcpy(buf, bus->fx_ui_hierarchy[k], len + 1); return len; }
                }
                /* fall through to the plugin */
            }
            if (strcmp(fxsub, "chain_params") == 0) {
                if (bus->fx_plugins_v2[k] && bus->fx_instances[k] &&
                    bus->fx_plugins_v2[k]->get_param) {
                    int r = bus->fx_plugins_v2[k]->get_param(bus->fx_instances[k],
                                                             fxsub, buf, buf_len);
                    /* An empty array is NOT an answer — see
                     * chain_params_answer_is_useful for the module that ships
                     * "[]" and the knobs it broke. */
                    if (chain_params_answer_is_useful(buf, r)) return r;
                }
                if (bus->fx_params[k] && bus->fx_param_counts[k] > 0)
                    return chain_params_emit_json(bus->fx_params[k],
                                                  bus->fx_param_counts[k], buf, buf_len);
                return -1;
            }
            if (bus->fx_plugins_v2[k] && bus->fx_instances[k] &&
                bus->fx_plugins_v2[k]->get_param)
                return bus->fx_plugins_v2[k]->get_param(bus->fx_instances[k],
                                                        fxsub, buf, buf_len);
            return -1;
        }
    }
    return -1;
}

int chain_bus_slot_get_param(chain_instance_t *inst, const char *sub, char *buf, int buf_len)
{
    if (!inst || !sub || !buf || buf_len <= 0) return -1;
    if (strcmp(sub, "config") == 0) return bus_emit_config(inst, buf, buf_len);
    int s = bus_suffix_index(sub, "main_send", BUS_MIX_SENDS);
    if (s > 0) return snprintf(buf, buf_len, "%d", inst->main_send_level[s - 1]);
    if (strcmp(sub, "orphans") == 0) {
        int total = 0;
        for (int b = 0; b < SLOT_BUSES; b++) total += inst->buses[b].orphan_count;
        return snprintf(buf, buf_len, "%d", total);
    }
    return -1;
}

/* ============================================================================
 * RT: patch apply
 * ============================================================================ */

int chain_bus_apply_patch(chain_instance_t *inst, const patch_info_t *patch)
{
    if (!inst || !patch) return 0;
    for (int b = 0; b < SLOT_BUSES; b++) {
        const bus_config_t *cfg = &patch->buses[b];
        slot_bus_t *bus = &inst->buses[b];
        if (!cfg->present) { bus_reset(inst, b); continue; }

        bus->in_use = 1;
        strncpy(bus->name, cfg->name, MAX_NAME_LEN - 1);
        bus->name[MAX_NAME_LEN - 1] = '\0';
        if (!bus->name[0]) snprintf(bus->name, sizeof(bus->name), "Bus %d", b + 1);

        int n = cfg->voice_id_count;
        if (n > SPLIT_VOICES_MAX) n = SPLIT_VOICES_MAX;
        if (n < 0) n = 0;
        memset(bus->voice_ids, 0, sizeof(bus->voice_ids));
        for (int i = 0; i < n; i++) {
            strncpy(bus->voice_ids[i], cfg->voice_ids[i], SPLIT_VOICE_ID_LEN - 1);
            bus->voice_ids[i][SPLIT_VOICE_ID_LEN - 1] = '\0';
        }
        bus->voice_id_count = n;

        for (int i = 0; i < BUS_MIX_SENDS; i++)
            bus->send_level[i] = bus_clamp_send(cfg->sends[i]);

        for (int i = 0; i < MAX_AUDIO_FX; i++) {
            const bus_fx_config_t *fx = &cfg->fx[i];
            const char *want = (i < cfg->fx_count) ? fx->module : "";
            if (want[0] && !bus_valid_module_name(want)) want = "";
            strncpy(bus->fx_request[i], want, MAX_NAME_LEN - 1);
            bus->fx_request[i][MAX_NAME_LEN - 1] = '\0';
            bus->fx_bypassed[i] = (i < cfg->fx_count && fx->bypassed) ? 1 : 0;
            /*
             * STATE, NEVER SHAPE. The state is staged unconditionally, but the
             * worker only DESTROYS AND RECREATES a position whose module name
             * actually changed — so restoring a kit whose reverb is already
             * loaded re-applies its parameters without cutting its tail.
             */
            if (i < cfg->fx_count && fx->state[0]) {
                strncpy(bus->fx_state_request[i], fx->state, MAX_BUS_FX_STATE_LEN - 1);
                bus->fx_state_request[i][MAX_BUS_FX_STATE_LEN - 1] = '\0';
                __atomic_store_n(&bus->fx_state_pending[i], 1, __ATOMIC_RELEASE);
            }
        }
        /* One request per bus, after every field is written: the worker's
         * ACQUIRE of fx_req_seq is what makes all of the above visible to it. */
        if (!__atomic_load_n(&bus->buf, __ATOMIC_RELAXED))
            chain_bus_request_alloc(inst, b);
        bus_request_work(inst, b);
    }

    for (int i = 0; i < BUS_MIX_SENDS; i++)
        inst->main_send_level[i] = bus_clamp_send(patch->main_sends[i]);

    chain_bus_rebuild_voice_map(inst);

    int orphans = 0;
    for (int b = 0; b < SLOT_BUSES; b++) orphans += inst->buses[b].orphan_count;
    return orphans;
}

/* ============================================================================
 * Worker: the only place a bus allocates, opens a file or dlopens
 * ============================================================================ */

void chain_bus_free_fx_meta(slot_bus_t *bus, int pos)
{
    free(bus->fx_params[pos]);       bus->fx_params[pos] = NULL;
    free(bus->fx_ui_hierarchy[pos]); bus->fx_ui_hierarchy[pos] = NULL;
    bus->fx_param_counts[pos] = 0;
}

static void bus_unload_fx(slot_bus_t *bus, int pos)
{
    if (bus->fx_plugins_v2[pos] && bus->fx_instances[pos] &&
        bus->fx_plugins_v2[pos]->destroy_instance)
        bus->fx_plugins_v2[pos]->destroy_instance(bus->fx_instances[pos]);
    bus->fx_instances[pos] = NULL;
    bus->fx_plugins_v2[pos] = NULL;
    if (bus->fx_handles[pos]) { dlclose(bus->fx_handles[pos]); bus->fx_handles[pos] = NULL; }
    bus->current_fx_modules[pos][0] = '\0';
    chain_bus_free_fx_meta(bus, pos);
}

/*
 * Load one bus FX position. WORKER ONLY.
 *
 * Every expensive thing this feature does is in this function: a dlopen, a
 * create_instance, a ~1.1 MB chain_param_info_t table and a 64 KB ui_hierarchy
 * cache, plus the module.json read behind both. That is the whole reason the
 * worker exists — chain_host.c's v2_load_audio_fx_slot does all of this on the
 * SPI callback, so the surrounding code is not a guide here.
 *
 * Pointers are stored AS SOON AS THEY EXIST rather than at the end, so a
 * teardown that interrupts this leaves chain_bus_release_all something it can
 * free. Nothing published here is legible to the render path until the caller
 * stores fx_ready.
 */
static int bus_load_fx(chain_instance_t *inst, slot_bus_t *bus, int pos, const char *name)
{
    char path[MAX_PATH_LEN], dir[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/../audio_fx/%s/%s.so", inst->module_dir, name, name);
    snprintf(dir, sizeof(dir), "%s/../audio_fx/%s", inst->module_dir, name);

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return -1;
    audio_fx_init_v2_fn init_v2 = (audio_fx_init_v2_fn)dlsym(handle, AUDIO_FX_INIT_V2_SYMBOL);
    if (!init_v2) { dlclose(handle); return -1; }
    audio_fx_api_v2_t *api = init_v2(&inst->subplugin_host_api);
    if (!api || api->api_version != AUDIO_FX_API_VERSION_2 || !api->create_instance) {
        dlclose(handle); return -1;
    }
    void *fx = api->create_instance(dir, NULL);
    if (!fx) { dlclose(handle); return -1; }

    bus->fx_handles[pos] = handle;
    bus->fx_plugins_v2[pos] = api;
    bus->fx_instances[pos] = fx;
    strncpy(bus->current_fx_modules[pos], name, MAX_NAME_LEN - 1);
    bus->current_fx_modules[pos][MAX_NAME_LEN - 1] = '\0';

    /* Metadata is allocated PER OCCUPIED POSITION. Eagerly for all
     * SLOT_BUSES * MAX_AUDIO_FX would be ~145 MB across four slots — see
     * slot_bus_t::fx_params. A failure here is not fatal: the position runs
     * without cached metadata and get_param falls back to the plugin. */
    if (!bus->fx_params[pos])
        bus->fx_params[pos] = (chain_param_info_t *)calloc(MAX_CHAIN_PARAMS,
                                                           sizeof(chain_param_info_t));
    if (!bus->fx_ui_hierarchy[pos])
        bus->fx_ui_hierarchy[pos] = (char *)calloc(1, CHAIN_UI_HIERARCHY_LEN);
    bus->fx_param_counts[pos] = 0;
    if (bus->fx_params[pos])
        parse_chain_params(dir, bus->fx_params[pos], &bus->fx_param_counts[pos]);
    if (bus->fx_ui_hierarchy[pos])
        parse_ui_hierarchy_cache(dir, bus->fx_ui_hierarchy[pos], CHAIN_UI_HIERARCHY_LEN);
    return 0;
}

void chain_bus_worker_reconcile(chain_instance_t *inst, int b, const int *run_flag)
{
    slot_bus_t *bus = &inst->buses[b];

    /* The seq we are answering. Everything the RT thread wrote before bumping
     * it is visible after this ACQUIRE. */
    unsigned seq = __atomic_load_n(&bus->fx_req_seq, __ATOMIC_ACQUIRE);

    /* A buffer the RT thread has already unpublished. Freeing it is safe for
     * exactly that reason and for no other. */
    int16_t *retired = __atomic_exchange_n(&bus->buf_retired, NULL, __ATOMIC_ACQ_REL);
    free(retired);

    if (bus->in_use && !__atomic_load_n(&bus->buf, __ATOMIC_RELAXED)) {
        /* BUS_BUF_SAMPLES, never a restated FRAMES_PER_BLOCK * 2: the render
         * path sizes every memset/memcpy off that same name with no bounds
         * check of its own. */
        int16_t *nb = (int16_t *)calloc(BUS_BUF_SAMPLES, sizeof(int16_t));
        /* Publish LAST with RELEASE; v2_render_block loads it ACQUIRE. A failed
         * calloc is not latched — the bus plays through Main and the next
         * request retries. */
        if (nb) __atomic_store_n(&bus->buf, nb, __ATOMIC_RELEASE);
    }

    int last = -1;
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        /* BETWEEN UNITS OF WORK. v2_destroy_instance's pthread_join blocks the
         * SPI callback, and pthread_join is a futex wait with no priority
         * inheritance — so the join must not have to wait out a whole queue of
         * dlopens. Checking here bounds it at one position. Whatever has
         * already been stored above is freed by chain_bus_release_all. */
        if (run_flag && !__atomic_load_n(run_flag, __ATOMIC_ACQUIRE)) return;

        const char *want = bus->fx_request[i];
        if (!want[0]) {
            if (bus->fx_handles[i] || bus->fx_instances[i]) bus_unload_fx(bus, i);
            continue;
        }
        if (strcmp(bus->current_fx_modules[i], want) != 0) {
            /* SHAPE changed: this is the one path that reinstantiates. */
            bus_unload_fx(bus, i);
            if (bus_load_fx(inst, bus, i, want) != 0) continue;
        }
        /* STATE, applied whether or not the instance is new — which is what
         * lets a restore reach a running FX without rebuilding it. */
        if (__atomic_exchange_n(&bus->fx_state_pending[i], 0, __ATOMIC_ACQ_REL)) {
            if (bus->fx_plugins_v2[i] && bus->fx_instances[i] &&
                bus->fx_plugins_v2[i]->set_param)
                bus->fx_plugins_v2[i]->set_param(bus->fx_instances[i], "state",
                                                 bus->fx_state_request[i]);
        }
        if (bus->fx_instances[i]) last = i;
    }
    bus->fx_count = last + 1;

    /*
     * Publish only if the request has not moved under us. Otherwise leave
     * fx_ready at 0 and let the post that accompanied the newer seq bring us
     * back — the RT side never waits, and a half-reconciled chain is never
     * announced as ready.
     */
    if (__atomic_load_n(&bus->fx_req_seq, __ATOMIC_ACQUIRE) == seq)
        __atomic_store_n(&bus->fx_ready, 1, __ATOMIC_RELEASE);
}
