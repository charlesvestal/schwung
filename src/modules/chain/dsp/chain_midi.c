/*
 * Signal Chain — MIDI clock state, MIDI FX hosting, v2 MIDI input path.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"
#include "chain_pre_inject.h"
#include "chain_midi_chain.h"

/* Clock availability state for sync-aware MIDI FX (arp, etc.). */
static int g_clock_output_enabled = 1;              /* midiClockMode == "output" */
static int g_clock_transport_running = 0;           /* Start/Continue seen without Stop */
static uint64_t g_clock_last_tick_ms = 0;           /* Last 0xF8 tick timestamp */
static uint64_t g_clock_next_refresh_ms = 0;        /* Settings.json refresh gate */

static int chain_read_clock_output_enabled(void) {
    FILE *f = fopen(MOVE_SETTINGS_JSON_PATH, "r");
    if (!f) return 1;  /* Avoid false warnings if settings file is unavailable. */

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > CLOCK_SETTINGS_MAX_BYTES) {
        fclose(f);
        return 1;
    }

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return 1;
    }

    size_t nread = fread(json, 1, (size_t)size, f);
    if (nread == 0 && ferror(f)) {
        free(json);
        fclose(f);
        return 1;
    }
    json[nread] = '\0';
    fclose(f);

    const char *key = "\"midiClockMode\"";
    char *pos = strstr(json, key);
    if (!pos) {
        free(json);
        return 1;
    }

    pos = strchr(pos + strlen(key), ':');
    if (!pos) {
        free(json);
        return 1;
    }

    while (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != '"') {
        free(json);
        return 1;
    }
    pos++;

    char mode[32];
    int i = 0;
    while (*pos && *pos != '"' && i < (int)sizeof(mode) - 1) {
        mode[i++] = *pos++;
    }
    mode[i] = '\0';

    free(json);

    if (strcmp(mode, "output") == 0) return 1;
    if (strcmp(mode, "off") == 0) return 0;
    if (strcmp(mode, "input") == 0) return 0;
    return 1;  /* Unknown value: avoid false warnings. */
}

/*
 * Diagnostic: what actually reaches the synth, and how many MIDI FX the chain
 * thinks it has.
 *
 * `touch /data/UserData/schwung/chain_midi_trace_on` -> lines in debug.log:
 *
 *   chain_trace IN 90 47 3b  midi_fx_count=1 -> out=6
 *   chain_trace   -> synth 90 35 7f
 *
 * OFF BY DEFAULT and it must stay that way: this runs on the SPI callback and
 * host->log is unified_log, which is not realtime-safe. The flag file is
 * stat()ed once every CHAIN_TRACE_CHECK_CALLS calls rather than per event, and
 * NOTE EVENTS ONLY are traced -- tracing clock would be 24 lines a beat and
 * would itself cause the dropouts it is being used to investigate (see the
 * debug_log_on lesson). Remember to remove the flag file when finished.
 *
 * Added while chasing the impressive-chords dry note, after three hypotheses
 * that each fitted the symptom and each turned out to be wrong. The rule from
 * that hunt applies here: read the values, do not infer them.
 */
#define CHAIN_TRACE_CHECK_CALLS 512
static int chain_midi_trace_enabled(void) {
    static int enabled = 0;
    static int counter = 0;
    if (counter++ % CHAIN_TRACE_CHECK_CALLS == 0) {
        enabled = (access("/data/UserData/schwung/chain_midi_trace_on", F_OK) == 0);
    }
    return enabled;
}

static int chain_midi_is_note(const uint8_t *msg, int len) {
    if (!msg || len < 3) return 0;
    int t = msg[0] & 0xF0;
    return t == 0x90 || t == 0x80;
}

static void chain_midi_trace(const chain_instance_t *inst, const char *what,
                             const uint8_t *msg, int len, int a, int b) {
    if (!inst || !inst->host || !inst->host->log) return;
    char line[128];
    if (a >= 0) {
        snprintf(line, sizeof(line), "chain_trace %s %02x %02x %02x  midi_fx_count=%d -> out=%d",
                 what, msg[0], msg[1], len > 2 ? msg[2] : 0, a, b);
    } else {
        snprintf(line, sizeof(line), "chain_trace %s %02x %02x %02x",
                 what, msg[0], msg[1], len > 2 ? msg[2] : 0);
    }
    inst->host->log(line);
}

static void chain_refresh_clock_output_enabled(uint64_t now_ms) {
    if (now_ms < g_clock_next_refresh_ms) return;
    g_clock_output_enabled = chain_read_clock_output_enabled();
    g_clock_next_refresh_ms = now_ms + CLOCK_SETTINGS_REFRESH_MS;
}

int chain_get_clock_status(void) {
    uint64_t now_ms = get_time_ms();
    chain_refresh_clock_output_enabled(now_ms);

    /* Move's internal sequencer clock now reaches us on cable 0 regardless of
     * the MIDI Clock Out setting (the shim broadcasts cable-0 realtime to all
     * slots). Derive transport state from actual clock activity, not from the
     * Clock Out preference — otherwise, with Clock Out off, we'd always report
     * UNAVAILABLE and clock-driven plugins (e.g. breakbeat treats UNAVAILABLE
     * as "keep running") would never see transport STOP. */
    int have_clock = (g_clock_last_tick_ms > 0) &&
                     ((now_ms - g_clock_last_tick_ms) <= CLOCK_TICK_STALE_MS);

    if (g_clock_transport_running && have_clock) {
        return MOVE_CLOCK_STATUS_RUNNING;
    }

    /* Ticks aren't arriving. If we've ever seen the transport (a prior tick) or
     * Clock Out is explicitly enabled, this is a real STOP. Stop is reported
     * instantly when Move emits 0xFC on cable 0, otherwise within
     * CLOCK_TICK_STALE_MS via tick staleness. Only when no clock has EVER
     * arrived AND Clock Out is off do we genuinely not know — report
     * UNAVAILABLE so the plugin free-runs (legacy behaviour). */
    if (g_clock_last_tick_ms > 0 || g_clock_output_enabled) {
        return MOVE_CLOCK_STATUS_STOPPED;
    }

    return MOVE_CLOCK_STATUS_UNAVAILABLE;
}

static void chain_update_clock_runtime(const uint8_t *msg, int len) {
    if (!msg || len < 1) return;

    uint8_t status = msg[0];
    uint64_t now_ms = get_time_ms();

    if (status == 0xF8) {          /* MIDI Clock tick */
        g_clock_last_tick_ms = now_ms;
    } else if (status == 0xFA || status == 0xFB) {  /* Start / Continue */
        g_clock_transport_running = 1;
        if (g_clock_last_tick_ms == 0) g_clock_last_tick_ms = now_ms;
    } else if (status == 0xFC) {   /* Stop */
        g_clock_transport_running = 0;
    }
}


/* Forward decls — definitions live further down with the v2 on_midi helpers. */
static inline void pre_mode_track_inject(chain_instance_t *inst,
                                         const uint8_t *out_msg, int out_len);

/*
 * Load a MIDI FX plugin into the slot it is addressed to.
 *
 * Mirrors v2_load_audio_fx_slot in chain_host.c, deliberately, down to the
 * count rule: `midi_fx_count` is a HIGH-WATER MARK, not a compacted length.
 * "midi_fx4:module" on an empty chain gives count 4 with slots 0-2 NULL, and
 * both loops that walk the list (v2_process_midi_fx, v2_tick_midi_fx) skip a
 * NULL plugin rather than stopping at it. Holes are legal on this side for the
 * same reason they are legal on the audio side: an eight-slot list with a
 * reorder UI hands out sparse writes as an ordinary event, and compacting on
 * the DSP side would silently renumber slots out from under whoever is holding
 * an index — the modulation matrix, the UI, the patch file.
 *
 * SLOT 1 IS NOT SPECIAL. It used to be: "midi_fx1:module" called
 * v2_unload_all_midi_fx first, on the rule that slot 1 owned the whole list.
 * That was harmless at a cap of 2 and is not at a cap of 8, where it destroys
 * up to seven neighbours. It also made a whole-chain rewrite depend on write
 * ORDER: writing midi_fx1:, midi_fx2:, midi_fx3: in sequence happened to work
 * only because the first write cleared the list and the rest appended; the
 * same three keys in any other order lost FX. Per-slot replace is order-free.
 *
 * Nothing in the field changes: the shadow UI only ever wrote midi_fx1:module
 * against a single-MIDI-FX chain, and there "unload all then load" and
 * "replace slot 0" are the same operation.
 *
 * What a caller must now do that it did not before: a rewrite that SHORTENS
 * the chain has to clear the tail explicitly ("midi_fx<N>:module=none" for
 * each dropped slot). The old nuke covered that by accident. The audio FX side
 * has always had this contract; this makes the two agree.
 */
int v2_load_midi_fx_slot(chain_instance_t *inst, int slot, const char *fx_name) {
    char msg[256];

    if (!inst || slot < 0 || slot >= MAX_MIDI_FX) return -1;

    /* Whatever was here goes first — including on the failure paths below, so
     * a bad name leaves a clean slot rather than a half-loaded one. */
    v2_unload_midi_fx_slot(inst, slot);

    /* Empty or "none" means just clear. Trailing empties shrink the mark so a
     * cleared tail does not leave the list reporting a length it no longer
     * has; an interior hole leaves it alone. */
    if (!fx_name || !fx_name[0] || strcmp(fx_name, "none") == 0) {
        while (inst->midi_fx_count > 0 && inst->midi_fx_handles[inst->midi_fx_count - 1] == NULL) {
            inst->midi_fx_count--;
        }
        snprintf(msg, sizeof(msg), "MIDI FX slot %d cleared", slot);
        v2_chain_log(inst, msg);
        return 0;
    }

    /* Build path to MIDI FX - in modules/midi_fx/ */
    char fx_path[MAX_PATH_LEN];
    char fx_dir[MAX_PATH_LEN];
    snprintf(fx_path, sizeof(fx_path), "%s/../midi_fx/%s/dsp.so",
             inst->module_dir, fx_name);
    snprintf(fx_dir, sizeof(fx_dir), "%s/../midi_fx/%s", inst->module_dir, fx_name);

    snprintf(msg, sizeof(msg), "Loading MIDI FX: %s", fx_path);
    v2_chain_log(inst, msg);

    /* Open the shared library */
    void *handle = dlopen(fx_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dlerror());
        v2_chain_log(inst, msg);
        return -1;
    }

    /* Look for init function */
    midi_fx_init_fn init_fn = (midi_fx_init_fn)dlsym(handle, MIDI_FX_INIT_SYMBOL);
    if (!init_fn) {
        snprintf(msg, sizeof(msg), "MIDI FX %s missing init symbol", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    midi_fx_api_v1_t *api = init_fn(&inst->subplugin_host_api);
    if (!api || api->api_version != MIDI_FX_API_VERSION) {
        snprintf(msg, sizeof(msg), "MIDI FX %s API version mismatch", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    void *instance = api->create_instance(fx_dir, NULL);
    if (!instance) {
        snprintf(msg, sizeof(msg), "MIDI FX %s create_instance failed", fx_name);
        v2_chain_log(inst, msg);
        dlclose(handle);
        return -1;
    }

    inst->midi_fx_handles[slot] = handle;
    inst->midi_fx_plugins[slot] = api;
    inst->midi_fx_instances[slot] = instance;
    inst->mod_param_refresh_ms_midi_fx[slot] = 0;
    strncpy(inst->current_midi_fx_modules[slot], fx_name, MAX_NAME_LEN - 1);
    inst->current_midi_fx_modules[slot][MAX_NAME_LEN - 1] = '\0';

    /* Parse chain_params from module.json for type info */
    if (parse_chain_params(fx_dir, inst->midi_fx_params[slot], &inst->midi_fx_param_counts[slot]) < 0) {
        v2_chain_log(inst, "ERROR: Failed to parse MIDI FX parameters");
        api->destroy_instance(instance);
        dlclose(handle);
        inst->midi_fx_handles[slot] = NULL;
        inst->midi_fx_plugins[slot] = NULL;
        inst->midi_fx_instances[slot] = NULL;
        inst->current_midi_fx_modules[slot][0] = '\0';
        return -1;
    }

    parse_ui_hierarchy_cache(fx_dir, inst->midi_fx_ui_hierarchy[slot], CHAIN_UI_HIERARCHY_LEN);

    /* Read optional "pre_capable" hint from module.json capabilities.
     * This informs the Shadow UI default on first placement; does not gate
     * the per-slot Pre/Post toggle (the user can still flip it manually). */
    inst->midi_fx_pre_capable[slot] = 0;
    char mj_path[MAX_PATH_LEN];
    snprintf(mj_path, sizeof(mj_path), "%s/module.json", fx_dir);
    FILE *mj = fopen(mj_path, "r");
    if (mj) {
        fseek(mj, 0, SEEK_END);
        long mj_size = ftell(mj);
        fseek(mj, 0, SEEK_SET);
        if (mj_size > 0 && mj_size < 65536) {
            char *mj_buf = malloc(mj_size + 1);
            if (mj_buf) {
                size_t nr = fread(mj_buf, 1, mj_size, mj);
                mj_buf[nr] = '\0';
                int cap = 0;
                if (json_get_int_in_section(mj_buf, "capabilities", "pre_capable", &cap) == 0
                    && cap) {
                    inst->midi_fx_pre_capable[slot] = 1;
                }
                free(mj_buf);
            }
        }
        fclose(mj);
    }

    if (slot >= inst->midi_fx_count) {
        inst->midi_fx_count = slot + 1;
    }

    snprintf(msg, sizeof(msg), "MIDI FX loaded: %s (slot %d)", fx_name, slot);
    v2_chain_log(inst, msg);
    return 0;
}

/*
 * Append at the end of the list. The patch loader wants this — it walks a
 * saved chain in order and relies on `midi_fx_count - 1` naming what it just
 * loaded — and so does anything that does not care where an FX lands.
 * Addressed writes go through v2_load_midi_fx_slot.
 */
int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name) {
    if (!inst || !fx_name || !fx_name[0]) return -1;
    if (inst->midi_fx_count >= MAX_MIDI_FX) {
        v2_chain_log(inst, "Max MIDI FX reached");
        return -1;
    }
    return v2_load_midi_fx_slot(inst, inst->midi_fx_count, fx_name);
}

/*
 * Tear down one slot. Does NOT touch midi_fx_count: the caller decides whether
 * the mark moves, because clearing an interior slot must not shorten the list
 * (see v2_load_midi_fx_slot).
 */
void v2_unload_midi_fx_slot(chain_instance_t *inst, int slot) {
    if (!inst || slot < 0 || slot >= MAX_MIDI_FX) return;

    char target[16];
    snprintf(target, sizeof(target), "midi_fx%d", slot + 1);
    chain_mod_clear_target_entries(inst, target, 0);

    if (inst->midi_fx_plugins[slot] && inst->midi_fx_instances[slot] &&
        inst->midi_fx_plugins[slot]->destroy_instance) {
        inst->midi_fx_plugins[slot]->destroy_instance(inst->midi_fx_instances[slot]);
    }
    if (inst->midi_fx_handles[slot]) {
        dlclose(inst->midi_fx_handles[slot]);
    }
    inst->midi_fx_handles[slot] = NULL;
    inst->midi_fx_plugins[slot] = NULL;
    inst->midi_fx_instances[slot] = NULL;
    inst->current_midi_fx_modules[slot][0] = '\0';
    inst->midi_fx_param_counts[slot] = 0;
    inst->mod_param_refresh_ms_midi_fx[slot] = 0;
    inst->midi_fx_ui_hierarchy[slot][0] = '\0';
    inst->midi_fx_pre_capable[slot] = 0;
    inst->midi_fx_bypassed[slot] = 0;

    /* Stale refcount entries from a now-unloaded MIDI FX would orphan future
     * note-ons. Reset with the FX, along with the pad-held tracker — whichever
     * FX replaces this one starts clean. Drop any buffered clock-driven inject
     * batch too, so the next clock doesn't flush the old FX's orphaned
     * note-ons into Move.
     *
     * This is chain-wide state, cleared on a per-slot unload. That is the
     * behaviour "midi_fx1:module" already had (it went through the unload-all
     * path), kept rather than narrowed: the tracker is not keyed by slot, so
     * there is nothing to clear selectively. The cost is that unloading one FX
     * can orphan a note another FX in the chain is still holding down — a
     * stuck note on Move until the next unload. Keying pre_injected_notes by
     * slot would fix it properly. */
    memset(inst->pre_injected_notes, 0, sizeof(inst->pre_injected_notes));
    memset(inst->pre_pad_held, 0, sizeof(inst->pre_pad_held));
    inst->pre_delay_count = 0;
}

/* Unload all MIDI FX from an instance */
void v2_unload_all_midi_fx(chain_instance_t *inst) {
    if (!inst) return;

    /* Every slot, not just the ones below the high-water mark. The mark is
     * bookkeeping and a failed load can leave it out of step with what is
     * actually held; sweeping the array cannot leak a dlopen handle. */
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        v2_unload_midi_fx_slot(inst, i);
    }
    inst->midi_fx_count = 0;
}

/* Process MIDI through all loaded MIDI FX modules */
static int v2_process_midi_fx(chain_instance_t *inst,
                              const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[],
                              int max_out) {
    if (!inst || inst->midi_fx_count == 0) {
        /* No MIDI FX - copy input to output */
        if (max_out > 0) {
            out_msgs[0][0] = in_msg[0];
            out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
            out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
            out_lens[0] = in_len;
            return 1;
        }
        return 0;
    }

    /* Process through chain of MIDI FX */
    uint8_t current[MIDI_FX_MAX_OUT_MSGS][3];
    int current_lens[MIDI_FX_MAX_OUT_MSGS];
    int current_count = 1;

    current[0][0] = in_msg[0];
    current[0][1] = in_len > 1 ? in_msg[1] : 0;
    current[0][2] = in_len > 2 ? in_msg[2] : 0;
    current_lens[0] = in_len;

    for (int fx = 0; fx < inst->midi_fx_count; fx++) {
        if (fx < MAX_MIDI_FX && inst->midi_fx_bypassed[fx]) continue;
        midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx];
        void *fx_inst = inst->midi_fx_instances[fx];
        if (!api || !fx_inst || !api->process_midi) continue;

        uint8_t next[MIDI_FX_MAX_OUT_MSGS][3];
        int next_lens[MIDI_FX_MAX_OUT_MSGS];
        int next_count = 0;

        /* Process each message from previous stage */
        for (int m = 0; m < current_count && next_count < MIDI_FX_MAX_OUT_MSGS; m++) {
            int room = MIDI_FX_MAX_OUT_MSGS - next_count;
            int out_count = api->process_midi(fx_inst,
                                              current[m], current_lens[m],
                                              &next[next_count], &next_lens[next_count],
                                              room);
            /* Believe the buffer, not the plugin: an over-reported count would
             * push next_count past the end of `next`/`current` and corrupt the
             * stack inside the audio callback. Same clamp, and same reasoning,
             * as chain_midi_run_from -- found by the test for that one. */
            if (out_count > room) out_count = room;
            if (out_count > 0) next_count += out_count;
        }

        /* Copy to current for next iteration */
        current_count = next_count;
        for (int i = 0; i < next_count; i++) {
            current[i][0] = next[i][0];
            current[i][1] = next[i][1];
            current[i][2] = next[i][2];
            current_lens[i] = next_lens[i];
        }
    }

    /* Copy final output */
    int out_count = 0;
    for (int i = 0; i < current_count && out_count < max_out; i++) {
        out_msgs[out_count][0] = current[i][0];
        out_msgs[out_count][1] = current[i][1];
        out_msgs[out_count][2] = current[i][2];
        out_lens[out_count] = current_lens[i];
        out_count++;
    }
    return out_count;
}

/* Call tick on all MIDI FX modules and send generated messages to synth */
/*
 * Run a message set through the MIDI FX chain STARTING AT `from`.
 *
 * This exists for GENERATED notes. An arpeggiator does not emit from
 * process_midi -- it holds the incoming note and produces on its own clock via
 * tick() -- and that output used to go straight to the synth, skipping every FX
 * after it. So two arps did not chain: both ran off the same input in parallel
 * and both fed the synth, which is not what a left-to-right chain promises.
 * Reported on hardware: "i'd expect arp 1 to output notes that are fed into
 * arp 2".
 *
 * Starting at `from` (the caller passes stage + 1) is what makes this safe: a
 * stage can never feed itself or anything upstream, so there is no cycle to
 * guard against and no need for a depth counter.
 *
 * Transforms in place and returns the new count. One `next` buffer is reused
 * across stages rather than nested, so the stack cost is flat in chain length
 * -- this runs in the audio callback.
 */
static int v2_stage_active(void *ctx, int stage) {
    chain_instance_t *inst = (chain_instance_t *)ctx;
    if (!inst) return 0;
    if (stage < MAX_MIDI_FX && inst->midi_fx_bypassed[stage]) return 0;
    midi_fx_api_v1_t *api = inst->midi_fx_plugins[stage];
    /* Holes are legal on this side -- an inactive stage is SKIPPED, never a
     * reason to stop walking the list. */
    return api && inst->midi_fx_instances[stage] && api->process_midi;
}

static int v2_stage_process(void *ctx, int stage, const uint8_t *in, int in_len,
                            uint8_t (*out)[3], int *out_lens, int max) {
    chain_instance_t *inst = (chain_instance_t *)ctx;
    return inst->midi_fx_plugins[stage]->process_midi(
        inst->midi_fx_instances[stage], in, in_len, out, out_lens, max);
}

static int v2_run_midi_fx_from(chain_instance_t *inst, int from,
                               uint8_t msgs[][3], int lens[], int count) {
    if (!inst) return count;
    uint8_t scratch[MIDI_FX_MAX_OUT_MSGS][3];
    int scratch_lens[MIDI_FX_MAX_OUT_MSGS];
    return chain_midi_run_from(from, inst->midi_fx_count,
                               v2_stage_active, v2_stage_process, inst,
                               msgs, lens, count,
                               scratch, scratch_lens, MIDI_FX_MAX_OUT_MSGS);
}

void v2_tick_midi_fx(chain_instance_t *inst, int frames) {
    if (!inst) return;

    for (int fx = 0; fx < inst->midi_fx_count; fx++) {
        if (fx < MAX_MIDI_FX && inst->midi_fx_bypassed[fx]) continue;
        midi_fx_api_v1_t *api = inst->midi_fx_plugins[fx];
        void *fx_inst = inst->midi_fx_instances[fx];
        if (!api || !fx_inst || !api->tick) continue;

        uint8_t out_msgs[MIDI_FX_MAX_OUT_MSGS][3];
        int out_lens[MIDI_FX_MAX_OUT_MSGS];
        int count = api->tick(fx_inst, frames, SAMPLE_RATE,
                              out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);

        /* Generated notes travel the REST of the chain, like any other note.
         * Without this an arp's output skipped every FX after it, so two arps
         * ran in parallel off the same input instead of feeding each other.
         * Downstream only (fx + 1), so a stage can never feed itself. */
        count = v2_run_midi_fx_from(inst, fx + 1, out_msgs, out_lens, count);

        /* Send generated messages to synth */
        for (int i = 0; i < count; i++) {
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
                /* Traced separately from the v2_on_midi path: a MIDI FX can
                 * emit here instead of there (impressive-chords queues all but
                 * the first release voice for tick), and a trace that covers
                 * only one of the two makes the other look like silence. */
                if (chain_midi_is_note(out_msgs[i], out_lens[i]) && chain_midi_trace_enabled())
                    chain_midi_trace(inst, "  ~> synth(tick)", out_msgs[i], out_lens[i], -1, 0);
                inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msgs[i], out_lens[i], 0);
            }
        }

        /* Pre mode: also inject into Move's MIDI_IN so Move's native
         * instrument on the slot's recv channel plays it additively. We
         * override the channel byte with the slot's live recv channel —
         * forward_channel is the synth-internal routing hint and must
         * never reach Move. */
        if (inst->midi_fx_pre_mode && inst->host && inst->host->midi_inject_to_move) {
            int recv_ch = -2;
            if (inst->host->slot_recv_channel) {
                recv_ch = inst->host->slot_recv_channel((void *)inst);
            }
            for (int i = 0; i < count; i++) {
                if (out_lens[i] < 1) continue;
                uint8_t type = out_msgs[i][0] & 0xF0;
                uint8_t cin;
                switch (type) {
                    case 0x80: cin = 0x08; break;  /* Note-off */
                    case 0x90: cin = 0x09; break;  /* Note-on */
                    case 0xA0: cin = 0x0A; break;  /* Poly AT */
                    case 0xB0: cin = 0x0B; break;  /* CC */
                    case 0xC0: cin = 0x0C; break;  /* Program */
                    case 0xD0: cin = 0x0D; break;  /* Channel AT */
                    case 0xE0: cin = 0x0E; break;  /* Pitch bend */
                    default:   continue;           /* Skip sysex/realtime */
                }
                /* Decide whether this FX-output message reaches Move: skip
                 * note-ONs on held-pad pitches (Move plays them natively) and
                 * skip note-OFFs for pitches we never injected (an arp
                 * retriggering a held pad would otherwise silence Move's pad
                 * note AND echo the off back un-refcounted, draining the arp).
                 * Pure + unit-tested: chain_pre_inject.h, tests/host/
                 * test_chain_pre_inject.c. */
                if (!chain_pre_tick_should_inject(inst->pre_pad_held,
                                                  inst->pre_injected_notes,
                                                  out_msgs[i], out_lens[i])) {
                    continue;
                }
                uint8_t inject_status = out_msgs[i][0];
                if (recv_ch >= 0 && recv_ch <= 15) {
                    inject_status = (inject_status & 0xF0) | (uint8_t)recv_ch;
                }
                uint8_t pkt[4] = { (2 << 4) | cin, inject_status, out_msgs[i][1], out_msgs[i][2] };
                if (inst->host->midi_inject_to_move(pkt, 4) > 0) {
                    pre_mode_track_inject(inst, out_msgs[i], out_lens[i]);
                }
            }
        }
    }
}

/* ==========================================================================
 * Per-instance MIDI FX helpers (for V2 API)
 * ========================================================================== */

/* Check whether an incoming note event is an echo of one we just injected
 * into Move's MIDI_IN (Pre mode). On match, note-off echoes decrement the
 * refcount so future note-ons on the same pitch are processed normally.
 * Returns 1 if the event should be dropped entirely (do not run MIDI FX,
 * do not dispatch to synth), 0 otherwise. */
static int pre_mode_is_echo(chain_instance_t *inst, const uint8_t *msg, int len) {
    if (!inst || !inst->midi_fx_pre_mode || len < 3) return 0;
    uint8_t type = msg[0] & 0xF0;
    if (type != 0x80 && type != 0x90) return 0;
    uint8_t note = msg[1];
    if (note >= 128) return 0;
    if (inst->pre_injected_notes[note] == 0) return 0;

    int is_note_off = (type == 0x80) || (type == 0x90 && msg[2] == 0);
    if (is_note_off) inst->pre_injected_notes[note]--;
    return 1;
}

/* Track one injected note-on so we can recognize its cable-2 echo and drop
 * it in pre_mode_is_echo. Called for every packet we write to the inject
 * SHM; note-off injections don't touch the refcount (the decrement happens
 * when the echo arrives). */
static inline void pre_mode_track_inject(chain_instance_t *inst,
                                         const uint8_t *out_msg, int out_len) {
    if (!inst || out_len < 3) return;
    uint8_t type = out_msg[0] & 0xF0;
    if (type != 0x90 || out_msg[2] == 0) return;  /* only real note-ons */
    uint8_t note = out_msg[1];
    if (note < 128 && inst->pre_injected_notes[note] < 255) {
        inst->pre_injected_notes[note]++;
    }
}

/* Inject one MIDI FX output message into Move's MIDI_IN (cable 2): rewrite the
 * channel byte to the slot's recv channel and bump the echo refcount so Move's
 * loopback is filtered. Shared by the immediate (note-driven) and delayed
 * (clock-driven) Pre-mode inject paths. */
static void pre_inject_one(chain_instance_t *inst, const uint8_t *m, int mlen, int recv_ch) {
    if (!inst || !inst->host || !inst->host->midi_inject_to_move || mlen < 1) return;
    uint8_t type = m[0] & 0xF0;
    uint8_t cin;
    switch (type) {
        case 0x80: cin = 0x08; break;
        case 0x90: cin = 0x09; break;
        case 0xA0: cin = 0x0A; break;
        case 0xB0: cin = 0x0B; break;
        case 0xC0: cin = 0x0C; break;
        case 0xD0: cin = 0x0D; break;
        case 0xE0: cin = 0x0E; break;
        default:   return;
    }
    uint8_t inject_status = m[0];
    if (recv_ch >= 0 && recv_ch <= 15) {
        inject_status = (uint8_t)((inject_status & 0xF0) | (uint8_t)recv_ch);
    }
    uint8_t pkt[4] = { (uint8_t)((2 << 4) | cin), inject_status, m[1], m[2] };
    if (inst->host->midi_inject_to_move(pkt, 4) > 0) {
        pre_mode_track_inject(inst, m, mlen);
    }
}

/* Flush the Pre-mode inject-delay buffer (the previous clock's clock-driven
 * output) into Move. Called at the start of each clock and on Stop. */
static void pre_delay_flush(chain_instance_t *inst) {
    for (int i = 0; i < inst->pre_delay_count; i++) {
        pre_inject_one(inst, inst->pre_delay_msg[i], inst->pre_delay_len[i],
                       inst->pre_delay_recv_ch);
    }
    inst->pre_delay_count = 0;
}

/* `inst_send_note_to_synth` lived here: a raw-msg-to-synth call with an optional
 * transpose, left over from the built-in chord FX. It had no callers, and it is
 * exactly the shape of bug that gets hunted through this file -- a route to the
 * synth that bypasses the MIDI FX chain. Deleted 2026-08-21 while proving the
 * impressive-chords dry note does NOT come from here; see
 * docs/plans/2026-08-21-impressive-chords-dry-note.md. Every remaining
 * synth->on_midi call site in this file is fed by v2_process_midi_fx output. */

/* V2 on_midi handler */
void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (!inst || len < 1) return;
    chain_update_clock_runtime(msg, len);

    /* Reset synced LFO phases on MIDI Start (0xFA) */
    if (len >= 1 && msg[0] == 0xFA) {
        for (int i = 0; i < LFO_COUNT; i++) {
            if (inst->lfos[i].sync) {
                inst->lfos[i].phase = 0.0;
            }
        }
    }

    /* LFO retrigger: reset phase on first note-on of new phrase */
    lfo_process_midi(inst->lfos, msg, len);

    /* FX broadcast: forward only to audio FX with on_midi (e.g. ducker).
     * Skip synth, MIDI FX, and knob handling - this MIDI is from a
     * different channel than the slot's target. */
    if (source == MOVE_MIDI_SOURCE_FX_BROADCAST) {
        for (int f = 0; f < inst->fx_count; f++) {
            if (inst->fx_on_midi[f] && inst->fx_instances[f]) {
                inst->fx_on_midi[f](inst->fx_instances[f], msg, len, source);
            }
        }
        return;
    }

    /* Pre-mode echo filter: drop cable-2 MIDI_OUT echoes of notes we just
     * injected into Move's MIDI_IN. Must run before MIDI FX processing or
     * chord/arp would transform and re-inject the echo → feedback loop.
     * The pad-originated event (what the user played) is not tracked, so
     * it passes through normally. */
    if (pre_mode_is_echo(inst, msg, len)) return;

    /* Pre-mode pad-held tracker: only real (non-echo) pad notes reach here.
     * Track so the tick-path can avoid injecting notes the user is
     * currently holding — otherwise the injection refcount for that pitch
     * would stay > 0 and the pad's note-off would be falsely filtered as
     * an echo. Updated after the echo filter so our own injections don't
     * pollute the set. */
    if (inst->midi_fx_pre_mode && len >= 3) {
        uint8_t type = msg[0] & 0xF0;
        uint8_t note = msg[1];
        if (note < 128) {
            if (type == 0x90 && msg[2] > 0) {
                if (inst->pre_pad_held[note] < 255) inst->pre_pad_held[note]++;
            } else if (type == 0x80 || (type == 0x90 && msg[2] == 0)) {
                if (inst->pre_pad_held[note] > 0) inst->pre_pad_held[note]--;
            }
        }
    }

    /* Handle knob CC mappings */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0) {
        uint8_t cc = msg[1];
        if (cc >= KNOB_CC_START && cc <= KNOB_CC_END) {
            for (int i = 0; i < inst->knob_mapping_count; i++) {
                if (inst->knob_mappings[i].cc == cc) {
                    const char *target = inst->knob_mappings[i].target;
                    const char *param = inst->knob_mappings[i].param;
                    chain_param_info_t *pinfo = knob_find_param(inst, target, param);
                    if (!pinfo) continue;

                    /* Calculate acceleration based on time between events */
                    uint64_t now = get_time_ms();
                    uint64_t last = inst->knob_last_time_ms[i];
                    inst->knob_last_time_ms[i] = now;
                    int accel = KNOB_ACCEL_MIN_MULT;
                    if (last > 0) {
                        uint64_t elapsed = now - last;
                        if (elapsed < KNOB_ACCEL_SLOW_MS) {
                            if (elapsed <= KNOB_ACCEL_FAST_MS) {
                                accel = KNOB_ACCEL_MAX_MULT;
                            } else {
                                float ratio = (float)(KNOB_ACCEL_SLOW_MS - elapsed) /
                                              (float)(KNOB_ACCEL_SLOW_MS - KNOB_ACCEL_FAST_MS);
                                accel = KNOB_ACCEL_MIN_MULT + (int)(ratio * (KNOB_ACCEL_MAX_MULT - KNOB_ACCEL_MIN_MULT));
                            }
                        }
                    }

                    /* Cap acceleration: enums never accelerate, ints limited */
                    int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);
                    if (pinfo->type == KNOB_TYPE_ENUM) {
                        accel = KNOB_ACCEL_ENUM_MULT;
                    } else if (is_int && accel > KNOB_ACCEL_MAX_MULT_INT) {
                        accel = KNOB_ACCEL_MAX_MULT_INT;
                    }

                    /* Relative encoder: apply acceleration to base step */
                    float base_step = (pinfo->step > 0) ? pinfo->step
                        : (is_int ? (float)KNOB_STEP_INT : KNOB_STEP_FLOAT);
                    float delta = 0.0f;
                    if (msg[2] == 1) {
                        delta = base_step * accel;
                    } else if (msg[2] == 127) {
                        delta = -base_step * accel;
                    } else {
                        return;  /* Ignore other values */
                    }

                    float new_val = inst->knob_mappings[i].current_value + delta;
                    if (new_val < pinfo->min_val) new_val = pinfo->min_val;
                    if (new_val > pinfo->max_val) new_val = pinfo->max_val;
                    if (is_int) new_val = (float)((int)new_val);  /* Round to int */
                    inst->knob_mappings[i].current_value = new_val;

                    /* Format as int or float */
                    char val_str[16];
                    if (is_int) {
                        snprintf(val_str, sizeof(val_str), "%d", (int)new_val);
                    } else {
                        snprintf(val_str, sizeof(val_str), "%.3f", new_val);
                    }

                    knob_forward_value(inst, target, param, val_str);
                    /* Relative: the sender moved by a delta and has no idea
                     * where that landed, so it needs the absolute answer. */
                    knob_emit_cc_out(inst, i);
                    return;
                }
            }
        }
        /* Absolute knob automation: CC 102-109 → knob 1-8 (0-127 scaled to param range).
         * Same knob mappings as CC 71-78, but interprets the value as an absolute
         * position rather than a relative encoder delta. Consumed here; not forwarded
         * to synth/FX. */
        if (cc >= KNOB_ABS_CC_START && cc <= KNOB_ABS_CC_END) {
            int target_cc = KNOB_CC_START + (cc - KNOB_ABS_CC_START);
            for (int i = 0; i < inst->knob_mapping_count; i++) {
                if (inst->knob_mappings[i].cc == target_cc) {
                    const char *target = inst->knob_mappings[i].target;
                    const char *param = inst->knob_mappings[i].param;
                    chain_param_info_t *pinfo = knob_find_param(inst, target, param);
                    if (!pinfo) return;

                    float abs_val = pinfo->min_val + ((float)msg[2] / 127.0f) * (pinfo->max_val - pinfo->min_val);
                    int is_int = (pinfo->type == KNOB_TYPE_INT || pinfo->type == KNOB_TYPE_ENUM);
                    if (is_int) abs_val = (float)((int)(abs_val + 0.5f));
                    if (abs_val < pinfo->min_val) abs_val = pinfo->min_val;
                    if (abs_val > pinfo->max_val) abs_val = pinfo->max_val;
                    inst->knob_mappings[i].current_value = abs_val;

                    char val_str[16];
                    if (is_int) snprintf(val_str, sizeof(val_str), "%d", (int)abs_val);
                    else        snprintf(val_str, sizeof(val_str), "%.3f", abs_val);

                    knob_forward_value(inst, target, param, val_str);
                    /* Deliberately no knob_emit_cc_out() here. The sender set
                     * this value, so echoing it back is at best redundant and
                     * at worst a loop: a controller configured to echo its own
                     * output (common — it is how a motorised unit hears itself)
                     * would fight the user's fingers mid-turn. Record what the
                     * sender already knows so later change detection is honest. */
                    inst->knob_mappings[i].last_cc_out = msg[2];
                    return;
                }
            }
            return;  /* CC 102-109 consumed even if unmapped — don't forward to synth */
        }
    }

    /* Process through MIDI FX modules (if any loaded) */
    uint8_t out_msgs[MIDI_FX_MAX_OUT_MSGS][3];
    int out_lens[MIDI_FX_MAX_OUT_MSGS];
    int out_count = v2_process_midi_fx(inst, msg, len, out_msgs, out_lens, MIDI_FX_MAX_OUT_MSGS);

    const int trace = chain_midi_is_note(msg, len) && chain_midi_trace_enabled();
    if (trace) chain_midi_trace(inst, "IN", msg, len, inst->midi_fx_count, out_count);

    /* Send processed messages to synth */
    for (int i = 0; i < out_count; i++) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            if (trace) chain_midi_trace(inst, "  -> synth", out_msgs[i], out_lens[i], -1, 0);
            inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msgs[i], out_lens[i], source);
        }
    }

    /* Pre mode: also inject into Move's MIDI_IN (cable 2) so Move's native
     * instrument on the slot's recv channel plays the transformed stream
     * additively. The channel byte must be the slot's RECV channel — not
     * the FX output's channel (which carries forward_channel from the
     * synth-side remap). forward_channel is purely a synth-internal hint
     * (e.g. minijv part 6); Move tracks listen on recv.
     *
     * Root-match skip: if an output note matches the input note exactly
     * (same status + data1), Move's pad already triggered it via the
     * cable-0 pad path. Injecting would double-trigger the same note on
     * the track instrument. Chord-style FX emit root+intervals; only the
     * intervals need injecting. */
    /* Clock/transport-driven generators (e.g. Beat Bank, breakbeat) emit their
     * notes in response to a 1-byte realtime message — Clock/Start/Continue/Stop
     * (len < 2) — not a pad. The tick path already injects such generator output
     * in Pre mode; mirror that here so on-clock chain generators reach Move's
     * track too. Stop (0xFC) must be included: it flushes note-offs, and
     * skipping them would strand held notes on Move's track. There is no pad
     * involved, so the root-match echo skip below stays note-input-only, and
     * pre_mode_track_inject still guards the cable-2 loopback. */
    int clock_gen = (len == 1 && (msg[0] == 0xF8u || msg[0] == 0xFAu ||
                                  msg[0] == 0xFBu || msg[0] == 0xFCu));
    if (inst->midi_fx_pre_mode && inst->host && inst->host->midi_inject_to_move
        && (len >= 2 || clock_gen)) {
        int recv_ch = -2;
        if (inst->host->slot_recv_channel) {
            recv_ch = inst->host->slot_recv_channel(instance);
        }
        if (clock_gen) {
            /* Record-align (inject-only): a clock-driven step fires on the same
             * 0xF8 that advances Move's step, so injecting it immediately makes
             * Move record it against the PREVIOUS step (one 16th early). Delay
             * the inject one clock so it lands after the advance — while the
             * synth send above stayed immediate, keeping local timing tight.
             *   Start (0xFA): downbeat injects now (Move is at step 0, no
             *                 advance yet); drop any stale buffer first.
             *   Clock/Continue (0xF8/0xFB): flush the prior clock, buffer this.
             *   Stop (0xFC): flush prior AND inject now — note-offs must not
             *                strand a held note on Move's track. */
            int is_start = (msg[0] == 0xFAu);
            int is_stop  = (msg[0] == 0xFCu);
            if (is_start) inst->pre_delay_count = 0;   /* fresh bar: no stale inject */
            else          pre_delay_flush(inst);        /* the prior clock's output */
            int immediate = is_start || is_stop;
            inst->pre_delay_recv_ch = recv_ch;
            for (int i = 0; i < out_count; i++) {
                if (out_lens[i] < 1) continue;
                if (immediate) {
                    pre_inject_one(inst, out_msgs[i], out_lens[i], recv_ch);
                } else if (inst->pre_delay_count < CHAIN_PRE_DELAY_MAX) {
                    int k = inst->pre_delay_count++;
                    inst->pre_delay_msg[k][0] = out_msgs[i][0];
                    inst->pre_delay_msg[k][1] = out_msgs[i][1];
                    inst->pre_delay_msg[k][2] = out_msgs[i][2];
                    inst->pre_delay_len[k]    = out_lens[i];
                }
            }
        } else {
            /* Note-driven (pad) Pre-mode inject: immediate. Skip events
             * identical to the input pad event — Move already triggered them
             * via the cable-0 pad path (injecting would double-trigger). */
            for (int i = 0; i < out_count; i++) {
                if (out_lens[i] < 1) continue;
                if (out_lens[i] >= 2 &&
                    out_msgs[i][0] == msg[0] && out_msgs[i][1] == msg[1]) {
                    continue;
                }
                pre_inject_one(inst, out_msgs[i], out_lens[i], recv_ch);
            }
        }
    }

    /* Forward MIDI to audio FX that have on_midi (e.g. ducker) */
    for (int f = 0; f < inst->fx_count; f++) {
        if (inst->fx_on_midi[f] && inst->fx_instances[f]) {
            for (int j = 0; j < out_count; j++) {
                inst->fx_on_midi[f](inst->fx_instances[f], out_msgs[j], out_lens[j], source);
            }
        }
    }
}

/* V2 set_param handler */

