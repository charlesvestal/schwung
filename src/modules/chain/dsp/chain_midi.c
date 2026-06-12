/*
 * Signal Chain — MIDI clock state, MIDI FX hosting, v2 MIDI input path.
 * Split from chain_host.c (2026-06 cleanup step 10); pure relocation,
 * no behavior change. Shared types/decls live in chain_internal.h.
 */

#include "chain_internal.h"

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

/* Load a MIDI FX plugin into an instance slot */
int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name) {
    char msg[256];

    if (!inst || !fx_name || !fx_name[0]) return -1;

    if (inst->midi_fx_count >= MAX_MIDI_FX) {
        v2_chain_log(inst, "Max MIDI FX reached");
        return -1;
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

    int slot = inst->midi_fx_count;

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

    parse_ui_hierarchy_cache(fx_dir, inst->midi_fx_ui_hierarchy[slot], sizeof(inst->midi_fx_ui_hierarchy[slot]));

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

    inst->midi_fx_count++;

    snprintf(msg, sizeof(msg), "MIDI FX loaded: %s (slot %d)", fx_name, slot);
    v2_chain_log(inst, msg);
    return 0;
}

/* Unload all MIDI FX from an instance */
void v2_unload_all_midi_fx(chain_instance_t *inst) {
    if (!inst) return;

    for (int i = 0; i < inst->midi_fx_count; i++) {
        char target[16];
        snprintf(target, sizeof(target), "midi_fx%d", i + 1);
        chain_mod_clear_target_entries(inst, target, 0);

        if (inst->midi_fx_plugins[i] && inst->midi_fx_instances[i] &&
            inst->midi_fx_plugins[i]->destroy_instance) {
            inst->midi_fx_plugins[i]->destroy_instance(inst->midi_fx_instances[i]);
        }
        if (inst->midi_fx_handles[i]) {
            dlclose(inst->midi_fx_handles[i]);
        }
        inst->midi_fx_handles[i] = NULL;
        inst->midi_fx_plugins[i] = NULL;
        inst->midi_fx_instances[i] = NULL;
        inst->current_midi_fx_modules[i][0] = '\0';
        inst->midi_fx_param_counts[i] = 0;
        inst->mod_param_refresh_ms_midi_fx[i] = 0;
        inst->midi_fx_ui_hierarchy[i][0] = '\0';
        inst->midi_fx_pre_capable[i] = 0;
        inst->midi_fx_bypassed[i] = 0;
    }
    inst->midi_fx_count = 0;

    /* Stale refcount entries from a now-unloaded MIDI FX would orphan
     * future note-ons. Reset with the FX chain, along with the pad-held
     * tracker — whichever FX replaces this one starts clean. */
    memset(inst->pre_injected_notes, 0, sizeof(inst->pre_injected_notes));
    memset(inst->pre_pad_held, 0, sizeof(inst->pre_pad_held));
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
            int out_count = api->process_midi(fx_inst,
                                              current[m], current_lens[m],
                                              &next[next_count], &next_lens[next_count],
                                              MIDI_FX_MAX_OUT_MSGS - next_count);
            next_count += out_count;
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

        /* Send generated messages to synth */
        for (int i = 0; i < count; i++) {
            if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
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
                /* Skip note-ONs on pitches currently held by a pad — Move's
                 * track already plays them natively, and injecting would
                 * bump pre_injected_notes so the pad-release note-off gets
                 * falsely echo-filtered (arp-hang bug). Do NOT apply this
                 * to note-offs: note-off injections don't touch the echo
                 * refcount, and suppressing them strands chord voices on
                 * Move's track when pre_pad_held has accumulated (it can
                 * leak > 0 when a real pad release gets caught by the echo
                 * filter and the decrement below never runs). */
                int is_note_on = (type == 0x90 && out_lens[i] >= 3 && out_msgs[i][2] > 0);
                if (is_note_on && out_msgs[i][1] < 128 &&
                    inst->pre_pad_held[out_msgs[i][1]] > 0) {
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

/* Send a note to synth with optional transposition (for chords) */
static void inst_send_note_to_synth(chain_instance_t *inst, const uint8_t *msg, int len, int source, int interval) {
    if (!inst || len < 3) return;

    uint8_t out_msg[3] = { msg[0], msg[1], msg[2] };

    if (interval != 0) {
        int transposed = (int)msg[1] + interval;
        if (transposed < 0 || transposed > 127) return;  /* Out of range */
        out_msg[1] = (uint8_t)transposed;
    }

    if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
        inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msg, len, source);
    }
}

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

    /* Send processed messages to synth */
    for (int i = 0; i < out_count; i++) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
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
    if (inst->midi_fx_pre_mode && inst->host && inst->host->midi_inject_to_move
        && len >= 2) {
        int recv_ch = -2;
        if (inst->host->slot_recv_channel) {
            recv_ch = inst->host->slot_recv_channel(instance);
        }
        for (int i = 0; i < out_count; i++) {
            if (out_lens[i] < 1) continue;

            /* Skip injection for events identical to the input pad event */
            if (out_lens[i] >= 2 &&
                out_msgs[i][0] == msg[0] && out_msgs[i][1] == msg[1]) {
                continue;
            }

            uint8_t type = out_msgs[i][0] & 0xF0;
            uint8_t cin;
            switch (type) {
                case 0x80: cin = 0x08; break;
                case 0x90: cin = 0x09; break;
                case 0xA0: cin = 0x0A; break;
                case 0xB0: cin = 0x0B; break;
                case 0xC0: cin = 0x0C; break;
                case 0xD0: cin = 0x0D; break;
                case 0xE0: cin = 0x0E; break;
                default:   continue;
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

