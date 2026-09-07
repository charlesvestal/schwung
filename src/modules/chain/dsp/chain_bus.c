/* _GNU_SOURCE before ANY include: the bus worker moved here from chain_host.c
 * needs CPU_ZERO/CPU_SET/sched_setaffinity, which sched.h only declares under
 * it. chain_host.c guards it the same way. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
 * THE SECOND GATE IS A SEQUENCE NUMBER, NOT A FLAG, and every read of it goes
 * through bus_fx_ready(). The RT side closes it by bumping fx_req_seq alone;
 * the worker opens it by publishing the seq it started its pass from. That is
 * one atomic on each side, so there is no window in which the worker can
 * observe the gate as "still mine to open" and then open it after the RT thread
 * has closed it. The earlier boolean form had exactly that window — the worker
 * compared fx_req_seq, was preempted, and stored a bare 1 over a clear the RT
 * thread had made in between, which put process_block() on the audio thread in
 * a race with the destroy_instance() and dlclose() of the very next reconcile.
 */
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
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
 * THE CLOSE. Advancing fx_req_seq makes bus_fx_ready() false from the very next
 * frame, because whatever the worker last published no longer equals it — and
 * this is the same thread the render runs on, so there is no in-flight reader
 * to wait for. Nothing writes fx_ready here: a single store that only the
 * worker makes, carrying the seq it answered, is what stops a preempted worker
 * re-opening a gate this call closed.
 *
 * 0 is skipped on wrap so it keeps meaning "the worker has published nothing",
 * which is what makes a freshly constructed (calloc'd) bus read as NOT ready.
 *
 * IT IS SPLIT OUT FROM THE POST FOR ONE REASON: **CLOSE BEFORE ANY POST**. A
 * caller that also wakes the worker for some other errand — an allocation —
 * must close first, or the worker can read the seq, start a reconcile, and
 * dlclose an instance the RT side still sees the gate as OPEN over. That is
 * exactly the use-after-free the sequence number replaced the boolean to
 * prevent, and chain_bus_apply_patch had it: chain_bus_request_alloc (which
 * posts) ran one statement BEFORE the bump. Any new caller pairing these two
 * halves by hand must keep that order; tests/host/test_bus_gate_ordering.sh
 * fails on a post that precedes its close.
 *
 * RT-safe: one store.
 */
static void bus_close_fx_gate(slot_bus_t *bus)
{
    unsigned next = bus->fx_req_seq + 1u;
    if (next == 0u) next = 1u;
    __atomic_store_n(&bus->fx_req_seq, next, __ATOMIC_RELEASE);
}

/*
 * Wake the worker for `b`, WITHOUT touching the gate.
 *
 * A slot with no buses must still cost NO THREAD. chain_bus_post_work starts
 * the worker lazily, so posting unconditionally would spawn one on every patch
 * load — chain_bus_apply_patch resets all SLOT_BUSES positions whether or not
 * the file mentioned any. Flag the request and leave: a bus that has never
 * existed has nothing to reconcile, and the flag is still set when some other
 * bus does start the worker.
 *
 * RT-safe: one store and a sem_post.
 */
static void bus_post_work(chain_instance_t *inst, int b)
{
    if (!inst->bus_worker_started && !inst->buses[b].in_use) {
        __atomic_store_n(&inst->bus_alloc_pending[b], 1, __ATOMIC_RELEASE);
        return;
    }
    chain_bus_post_work(inst, b);
}

/* Hand a bus to the worker: the close, then the post, in that order and never
 * the other. Every caller that changes the FX REQUEST uses this. */
static void bus_request_work(chain_instance_t *inst, int b)
{
    bus_close_fx_gate(&inst->buses[b]);
    bus_post_work(inst, b);
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

    /*
     * PER-VOICE SENDS DO NOT RESOLVE HERE ANY MORE, and their absence is the
     * point: the MODULE owns those levels now (voice_send_source.h), so there
     * is no id-keyed config of ours to re-point and nothing here that could go
     * stale against the voice list. The cache is cleared by
     * chain_voice_sends_load, on the one event that changes the list.
     */
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
         * alternative is a leak.
         *
         * WHY IT CANNOT HAPPEN, precisely: the worker DRAINS buf_retired at the
         * top of a reconcile, BEFORE it allocates, and it allocates only while
         * in_use — so a second non-NULL `buf` cannot come into existence until
         * the retired one has been taken. Reaching this free needs two
         * non-NULL bufs, i.e. an allocation the drain did not precede. (The
         * weaker "buf was already NULL for prev" is true but would not survive
         * reordering the reconcile's drain and its calloc, which is exactly
         * when someone will re-read this.) */
        if (prev) free(prev);
    }
    bus_request_work(inst, b);
}

void chain_bus_clear_all(chain_instance_t *inst)
{
    if (!inst) return;
    for (int b = 0; b < SLOT_BUSES; b++) bus_reset(inst, b);
    for (int i = 0; i < BUS_MIX_SENDS; i++) {
        inst->main_send_level[i] = 0;
        /* The LFO's offset goes with it. lfo_tick re-zeroes this every block, so
         * a stale value could only be heard on the blocks between a reset and
         * the next tick -- but a reset that leaves audio flowing through a send
         * the user just cleared is exactly the kind of gap nobody looks for. */
        inst->main_send_mod[i] = 0;
    }
    /* PER-VOICE SENDS ARE NOT CLEARED WITH THEM, because they are not ours to
     * clear: the module holds those levels and this verb empties the slot's
     * BUSES. Clearing the cache here would silence a send for one sweep and
     * then have it come straight back from the module, which reads as a
     * glitch and settles nothing. */
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
             * so a slot with no buses costs no thread.
             *
             * No FX change here, so the gate is not closed: this bus was just
             * reset, every fx_request is empty, and there is no instance a
             * reconcile could destroy under a reader. (The phrase is what
             * test_bus_gate_ordering.sh looks for — every posting call site
             * either closes first or says here why it need not.) */
            chain_bus_request_alloc(inst, b);
        } else if (!__atomic_load_n(&bus->buf, __ATOMIC_RELAXED)) {
            /* IN USE BUT UNALLOCATED: the worker's calloc failed. The reconcile
             * says a failed calloc "is not latched — the next request retries",
             * and this is what makes that true: without a post here the retry
             * had to wait for some unrelated verb, so a bus that lost its
             * buffer once played through Main until the user happened to change
             * something else. post_work, not request_work — nothing about the
             * FX chain changed, so the FX gate must not be closed. */
            chain_bus_post_work(inst, b);
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
            if (bus_fx_ready(bus) &&
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
    /* NO "voice<V>:send<M>" ROUTE. It was the host's write path onto a
     * per-voice send and it is gone with the faders that used it — a level
     * belongs to the module's own parameter now, written the way every other
     * one of its parameters is (`synth:<key>`). Leaving the route behind would
     * leave a second way to set the same number, and the one that wins would
     * be whichever happened last. */
    if (!inst || !sub) return -1;
    int s = bus_suffix_index(sub, "main_send", BUS_MIX_SENDS);
    if (s > 0) {
        inst->main_send_level[s - 1] = bus_clamp_send(val ? atoi(val) : 0);
        inst->dirty = 1;
        return 0;
    }
    /* There is no "clear" verb. chain_bus_clear_all is real and called (from
     * the slot teardown in chain_host.c) — what was dead is the param KEY
     * nothing ever wrote, and a verb that erases four sub-mixes is the last one
     * to leave lying around untested. */
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
    /*
     * NO "voice_sends" KEY. The host published one for as long as it owned
     * those levels; the module owns them now, so the only honest answer to
     * "what are this slot's per-voice sends" is the module's own parameters,
     * and a second copy here is a second source of truth for the UI to believe.
     */
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
    /* No per-bus "orphans" key, and no slot-wide one either. The COUNT still
     * matters — a partial restore that reports nothing is indistinguishable
     * from a working one, which is why bus_voice_apply returns it — but
     * "buses:config" already carries `orphans` for every bus in the one GET the
     * UI actually makes (busRowLabel draws the "!" from it). A second spelling
     * served nobody and cost a round trip to find out.
     *
     * The ids themselves are in "voices" below, because they are RETAINED
     * rather than dropped, and toggling one off is the only thing that clears
     * the count. */
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
            if (!bus_fx_ready(bus)) return -1;
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
            /* CLEAR FIRST, UNCONDITIONALLY. A state staged by an earlier load
             * that the worker has not consumed yet is not ours to keep: this
             * patch may point the position at a DIFFERENT module, and the
             * worker would then hand the new plugin the old one's blob. The
             * new patch is the authority for both halves or for neither. */
            __atomic_store_n(&bus->fx_state_pending[i], 0, __ATOMIC_RELAXED);
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
        /*
         * One request per bus, after every field is written: the worker's
         * ACQUIRE of fx_req_seq is what makes all of the above visible to it.
         *
         * CLOSE FIRST. chain_bus_request_alloc POSTS the worker, and it used to
         * run one statement ahead of the bump — so a worker that read the seq
         * in that window ran its whole reconcile, destroy_instance and dlclose
         * included, while the RT side still saw fx_ready == fx_req_seq and
         * called process_block on the instance being torn down. Narrow (it
         * needs buf == NULL with live FX) and exactly the use-after-free the
         * sequence gate exists to prevent.
         */
        bus_close_fx_gate(bus);
        if (!__atomic_load_n(&bus->buf, __ATOMIC_RELAXED))
            chain_bus_request_alloc(inst, b);
        bus_post_work(inst, b);
    }

    for (int i = 0; i < BUS_MIX_SENDS; i++)
        inst->main_send_level[i] = bus_clamp_send(patch->main_sends[i]);

    /*
     * NO PER-VOICE SENDS IN A PATCH. They travel inside the synth's own `state`
     * blob, which this document already carries and which the module reads
     * back itself — so a slot reload restores them by the same route every
     * other one of its parameters takes. Applying a second copy from here
     * would race the state load, and the loser would be silent.
     */

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
 * The handle, the api and the instance are stored TOGETHER, once all three
 * exist — every failure before that point dlcloses on its own way out, so
 * there is nothing half-stored for a teardown to find. (The comment here used
 * to claim each pointer was stored as soon as it existed, which the stores
 * below have never done.) After that point chain_bus_release_all owns them,
 * including the metadata blocks allocated further down. Nothing published here
 * is legible to the render path until the caller stores fx_ready.
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
     * Publish WHICH REQUEST we answered, not that we answered one.
     *
     * Unconditional on purpose: this store IS the check. If the RT thread moved
     * the request while we worked, `seq` is now stale, bus_fx_ready() compares
     * it against the newer fx_req_seq and stays shut, and the post that
     * accompanied that bump brings us back for another pass. There is no
     * load-then-store here to be preempted in the middle of, which is the whole
     * reason this is a number and not a 1.
     */
    __atomic_store_n(&bus->fx_ready, seq, __ATOMIC_RELEASE);
}

/* ============================================================================
 * Bus allocation, off the realtime thread — MOVED HERE from chain_host.c
 * ============================================================================
 *
 * Not a reorganisation for its own sake: tests/host/test_chain_host_file_split.sh
 * caps chain_host.c at 2900 lines, and the bus work put it at 2896 — four lines
 * of headroom, which the next merge from main spent. This block is bus
 * lifecycle and it belongs beside the rest of it.
 */
/* ============================================================================
 * Bus allocation, off the realtime thread
 * ============================================================================ */

/*
 * Bus allocation worker. SCHED_OTHER on cores 0-2.
 *
 * THREADS INHERIT THE CALLBACK'S PRIORITY. pthread_create is called from a
 * module entry point, i.e. from the SPI callback, so this thread starts at
 * SCHED_FIFO 70 — above Move's own `Link Main` at FIFO 35, which it would then
 * starve, producing exactly the dropouts going off-thread was meant to avoid.
 * Demoting is therefore the FIRST thing here, before the instance pointer is
 * even dereferenced.
 */
static void *chain_bus_worker_fn(void *arg) {
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);   /* core 3 is SPI's */
    sched_setaffinity(0, sizeof(set), &set);

    chain_instance_t *inst = (chain_instance_t *)arg;

    for (;;) {
        /* Parked, not polling: the wake comes from chain_bus_request_alloc or
         * from the stop below. sem_wait is restartable, so EINTR is a retry and
         * not an exit — exiting on a stray signal would leave later requests
         * unserved with nothing to report it. */
        while (sem_wait(&inst->bus_worker_sem) != 0) {
            if (errno == EINTR) continue;
            /* Anything else is unreachable (EINVAL needs a destroyed
             * semaphore, and sem_destroy only runs after the join) — but
             * falling through would spin this core forever, since the
             * started flag is still set and the outer loop would re-enter
             * immediately. Leave instead. */
            return NULL;
        }

        if (!__atomic_load_n(&inst->bus_worker_started, __ATOMIC_ACQUIRE)) break;

        for (int b = 0; b < SLOT_BUSES; b++) {
            /* The stop flag, checked BETWEEN buses as well as inside the
             * reconcile: v2_destroy_instance joins this thread from the SPI
             * callback, so the queue must be abandonable at every unit
             * boundary rather than run to completion. */
            if (!__atomic_load_n(&inst->bus_worker_started, __ATOMIC_ACQUIRE)) break;
            if (!__atomic_load_n(&inst->bus_alloc_pending[b], __ATOMIC_ACQUIRE)) continue;
            /* Clear BEFORE attempting, not after. Clearing afterwards
             * clobbers a request the RT thread made WHILE we were working:
             * it would set pending=1, we would store 0 over it, and the bus
             * would sit unreconciled until some later change. Clearing first
             * turns that race into a harmless duplicate pass — and the
             * reconcile is idempotent, which is what makes a duplicate free. */
            __atomic_store_n(&inst->bus_alloc_pending[b], 0, __ATOMIC_RELEASE);
            /*
             * EVERYTHING EXPENSIVE LIVES IN HERE, and that is the entire point
             * of this thread: the buffer calloc, the dlopen and
             * create_instance for each bus FX, and the ~1.1 MB of parameter
             * metadata plus 64 KB ui_hierarchy cache each of those needs. See
             * chain_bus.c for the ownership split and for the two
             * release/acquire gates (`buf` and `fx_ready`) that join the two
             * threads.
             */
            chain_bus_worker_reconcile(inst, b, &inst->bus_worker_started);
        }
    }
    return NULL;
}

/*
 * RT side: mark and return.
 *
 * No BUS memory is allocated here — that is the whole point of the worker.
 * The one deliberate exception is the first call's pthread_create, which
 * allocates the worker's stack and TLS and issues clone(2) on this callback.
 * It is once per slot, on a user gesture, and there is nowhere earlier to put
 * it without starting a thread for every slot that never makes a bus. Do not
 * read "nothing is allocated here" into this function and add a second thing:
 * a comment claiming a realtime guarantee the code does not give is a defect
 * this branch has already shipped three times.
 *
 * The worker is started LAZILY, on the first bus a slot ever creates, so the
 * common case — a slot with no buses — costs no thread at all.
 */
void chain_bus_request_alloc(chain_instance_t *inst, int bus) {
    if (!inst || bus < 0 || bus >= SLOT_BUSES) return;
    inst->buses[bus].in_use = 1;
    chain_bus_post_work(inst, bus);
}

/* The same handover without the claim — see the header. A delete must reach the
 * worker too, and must not resurrect in_use on its way there. */
void chain_bus_post_work(chain_instance_t *inst, int bus) {
    if (!inst || bus < 0 || bus >= SLOT_BUSES) return;

    __atomic_store_n(&inst->bus_alloc_pending[bus], 1, __ATOMIC_RELEASE);

    if (!inst->bus_worker_started) {
        if (!inst->bus_worker_sem_ok) {
            /* sem_init writes the struct in place — no allocation, no lock. */
            if (sem_init(&inst->bus_worker_sem, 0, 0) != 0) return;
            inst->bus_worker_sem_ok = 1;
        }
        __atomic_store_n(&inst->bus_worker_started, 1, __ATOMIC_RELEASE);
        if (pthread_create(&inst->bus_worker, NULL, chain_bus_worker_fn, inst) != 0) {
            /* No worker, no allocation, no crash: the bus keeps playing
             * through Main and a later request tries again. */
            __atomic_store_n(&inst->bus_worker_started, 0, __ATOMIC_RELEASE);
            return;
        }
    }
    sem_post(&inst->bus_worker_sem);
}

/* Stop and JOIN the worker. Runs on the callback (destroy_instance does), and
 * the join is why the wake is a semaphore rather than a sleep: the worker is
 * parked, so it observes the cleared flag as soon as it is posted instead of
 * sitting out the rest of a poll period with the audio thread waiting on it. */
void chain_bus_worker_stop(chain_instance_t *inst) {
    if (!inst) return;
    if (inst->bus_worker_started) {
        __atomic_store_n(&inst->bus_worker_started, 0, __ATOMIC_RELEASE);
        sem_post(&inst->bus_worker_sem);
        pthread_join(inst->bus_worker, NULL);
    }
    if (inst->bus_worker_sem_ok) {
        sem_destroy(&inst->bus_worker_sem);
        inst->bus_worker_sem_ok = 0;
    }
}

/*
 * Release everything a bus owns, in the reverse order it was acquired: the FX
 * instances, then their dlopen handles, then the buffer. Missing any one is a
 * leak plus a dangling handle the render path cannot detect.
 *
 * Only safe after chain_bus_worker_stop — the worker writes buses[].buf.
 */
void chain_bus_release_all(chain_instance_t *inst) {
    if (!inst) return;
    for (int b = 0; b < SLOT_BUSES; b++) {
        slot_bus_t *bus = &inst->buses[b];
        for (int i = 0; i < MAX_AUDIO_FX; i++) {
            if (bus->fx_plugins_v2[i] && bus->fx_instances[i] &&
                bus->fx_plugins_v2[i]->destroy_instance) {
                bus->fx_plugins_v2[i]->destroy_instance(bus->fx_instances[i]);
            }
            bus->fx_instances[i] = NULL;
            bus->fx_plugins_v2[i] = NULL;
            if (bus->fx_handles[i]) {
                dlclose(bus->fx_handles[i]);
                bus->fx_handles[i] = NULL;
            }
            bus->fx_bypassed[i] = 0;
            bus->current_fx_modules[i][0] = '\0';
            /* The per-position metadata the worker allocated. Missing it is a
             * ~1.1 MB leak per occupied position, which four slots of buses
             * makes large enough to matter. */
            chain_bus_free_fx_meta(bus, i);
        }
        bus->fx_count = 0;
        bus->fx_ready = 0;
        free(bus->buf);
        bus->buf = NULL;
        /* A buffer the RT side unpublished and the worker never got to. */
        free(bus->buf_retired);
        bus->buf_retired = NULL;
        bus->in_use = 0;
    }
}


/* ============================================================================
 * RT: the module-owned per-voice send levels
 *
 * The host reads these; it does not store them. See voice_send_source.h for the
 * declaration shape, the send-index rule and the range mapping — everything
 * arithmetic lives there so tests/host can compile and RUN it, which this
 * translation unit cannot.
 * ============================================================================
 */

/*
 * How many (voice, send) keys the background sweep asks for per frame.
 *
 * A module's get_param runs on the SPI callback, so the full table for a 32-pad
 * rack is 64 calls and cannot be a per-frame cost. Four spreads that over 16
 * frames (~0.36 s) at a few microseconds a frame — fine as a GROUND TRUTH,
 * which is all it is: a knob turn arms chain_voice_sends_touch and is answered
 * on the very next frame, so the sweep only has to catch what no write of ours
 * went past (a state blob restore, a preset load, a module moving its own
 * value). Those are all followed by silence, where a third of a second of
 * staleness is inaudible.
 */
#define VOICE_SEND_POLL_PER_FRAME 4

void chain_voice_sends_load(chain_instance_t *inst)
{
    if (!inst) return;

    /* CLEARED FIRST AND UNCONDITIONALLY, on the one event that changes the
     * voice list — the same rule synth_last_note and synth_split_voice_ids
     * follow two lines away in v2_load_synth. A level cached against the
     * previous module's render index would be a send on whatever voice now
     * holds that index. */
    memset(inst->voice_send, 0, sizeof(inst->voice_send));
    memset(inst->voice_send_tmpl, 0, sizeof(inst->voice_send_tmpl));
    for (int s = 0; s < BUS_MIX_SENDS; s++) {
        inst->voice_send_min[s] = 0.0f;
        inst->voice_send_max[s] = 0.0f;
        inst->voice_send_is_db[s] = 0;
        inst->voice_send_meta_ok[s] = 0;
    }
    inst->voice_send_tmpl_count = 0;
    inst->voice_send_poll_cursor = 0;
    inst->voice_send_resweep = 0;

    if (!inst->synth_plugin_v2 || !inst->synth_instance ||
        !inst->synth_plugin_v2->get_param) return;

    char decl[512];
    decl[0] = '\0';
    int got = inst->synth_plugin_v2->get_param(inst->synth_instance,
                                               "voice_send_params",
                                               decl, sizeof(decl));
    /* got <= 0 is the module ANSWERING that it declares none — a real answer,
     * and the correct one for every module in the fleet but dr32. It is not a
     * failed read: this is a direct in-process call. */
    if (got <= 0) return;
    decl[sizeof(decl) - 1] = '\0';   /* nothing NUL-terminates a plugin's buffer */

    int n = voice_send_params_parse(decl, inst->voice_send_tmpl,
                                    BUS_MIX_SENDS, VOICE_SEND_TMPL_LEN);
    if (n <= 0) {
        /* REFUSED WHOLE, never half-applied. More entries than there are sends,
         * an entry with no "{id}", an entry too long: every one of them means
         * the module and the host disagree about what was declared, and a
         * partial acceptance is that disagreement made silent. */
        memset(inst->voice_send_tmpl, 0, sizeof(inst->voice_send_tmpl));
        if (n < 0) v2_chain_log(inst, "ERROR: voice_send_params refused (bad declaration)");
        return;
    }
    inst->voice_send_tmpl_count = n;

    /* THE RANGE, resolved once. A module's send parameter is whatever the
     * module says it is — dr32's is dB over -70..+6 — and a host that assumed
     * 0..127 would mis-scale it by two orders of magnitude with nothing on
     * screen to say so. Not found means REFUSED, not guessed. */
    for (int s = 0; s < n; s++) {
        char meta[VOICE_SEND_KEY_LEN];
        chain_param_info_t *pi = NULL;
        if (voice_send_meta_key(inst->voice_send_tmpl[s], meta, sizeof(meta)))
            pi = find_param_info(inst->synth_params, inst->synth_param_count, meta);
        if (!pi && inst->synth_split_voice_count > 0) {
            /* The other spelling: a module that really does declare each
             * voice's key rather than one focus-addressed template. */
            char abs_key[VOICE_SEND_KEY_LEN];
            if (voice_send_key_build(inst->voice_send_tmpl[s],
                                     inst->synth_split_voice_ids[0],
                                     abs_key, sizeof(abs_key)))
                pi = find_param_info(inst->synth_params, inst->synth_param_count, abs_key);
        }
        if (!pi || !(pi->max_val > pi->min_val)) {
            v2_chain_log(inst, "voice_send_params: no range for a declared send; refused");
            continue;
        }
        inst->voice_send_min[s] = pi->min_val;
        inst->voice_send_max[s] = pi->max_val;
        inst->voice_send_is_db[s] = voice_send_unit_is_db(pi->unit);
        inst->voice_send_meta_ok[s] = 1;
    }
}

void chain_voice_sends_touch(chain_instance_t *inst, const char *subkey)
{
    if (!inst || !subkey || inst->voice_send_tmpl_count <= 0) return;
    for (int s = 0; s < inst->voice_send_tmpl_count; s++) {
        if (voice_send_key_has_suffix(inst->voice_send_tmpl[s], subkey)) {
            inst->voice_send_resweep = 1;
            return;
        }
    }
}

/*
 * Refresh the cache from the module.
 *
 * ONE (voice, send) PAIR IS ONE get_param. A key the module does not serve
 * LEAVES THE LEVEL ALONE — it is not an answer of zero, and letting a
 * non-answer become a level is how a send silently mutes. A hole in the voice
 * list (an entry the module published with no usable id) has no key to ask for
 * and is left at whatever load cleared it to.
 *
 * RT-safe: bounded, no allocation, no I/O. What it calls into is the module's
 * own get_param, which is on the SPI callback either way.
 */
void chain_voice_sends_poll(chain_instance_t *inst, int n_voices)
{
    if (!inst || inst->voice_send_tmpl_count <= 0) return;
    if (!inst->synth_plugin_v2 || !inst->synth_instance ||
        !inst->synth_plugin_v2->get_param) return;
    int nt = inst->voice_send_tmpl_count;
    if (nt > BUS_MIX_SENDS) nt = BUS_MIX_SENDS;
    if (n_voices > inst->synth_split_voice_count) n_voices = inst->synth_split_voice_count;
    if (n_voices > SPLIT_VOICES_MAX) n_voices = SPLIT_VOICES_MAX;
    if (n_voices <= 0) return;

    int space = n_voices * nt;
    int budget = inst->voice_send_resweep ? space : VOICE_SEND_POLL_PER_FRAME;
    inst->voice_send_resweep = 0;
    if (budget > space) budget = space;

    int cursor = inst->voice_send_poll_cursor;
    if (cursor < 0 || cursor >= space) cursor = 0;

    for (int k = 0; k < budget; k++) {
        int v = cursor / nt;
        int s = cursor - v * nt;
        cursor++;
        if (cursor >= space) cursor = 0;
        if (!inst->voice_send_meta_ok[s]) continue;
        const char *id = inst->synth_split_voice_ids[v];
        if (!id[0]) continue;
        char key[VOICE_SEND_KEY_LEN];
        if (!voice_send_key_build(inst->voice_send_tmpl[s], id, key, sizeof(key)))
            continue;
        char val[64];
        val[0] = '\0';
        int got = inst->synth_plugin_v2->get_param(inst->synth_instance, key,
                                                   val, sizeof(val));
        if (got <= 0) continue;          /* unserved is NOT a level of zero */
        val[sizeof(val) - 1] = '\0';
        int level = 0;
        if (voice_send_level_map(atof(val), inst->voice_send_min[s],
                                 inst->voice_send_max[s],
                                 inst->voice_send_is_db[s], &level))
            inst->voice_send[v][s] = (int8_t)level;
    }
    inst->voice_send_poll_cursor = cursor;
}
