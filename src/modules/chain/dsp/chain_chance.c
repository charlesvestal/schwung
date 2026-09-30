/*
 * Step chance: Elektron-style trig conditions on Move's notes.
 *
 * Set from the step menu (hold a step, press Menu) -- the shim resolves the
 * held step to Move's note ids and writes them here as `chance:notes`. Applied
 * in v2_on_midi, ahead of the LFO retrigger, MIDI FX and synth, by the pure
 * gate in host/step_chance_gate.h: a losing note-on and its note-off never
 * reach anything in the slot.
 *
 * Only SCHWUNG's instruments are affected. Move's own instrument on the track
 * still plays the note -- this sees the note after Move has played it.
 *
 * Every entry point here runs on the SPI callback except the document parse
 * and serialize, which the UI's autosave/restore drive (same as lanes:state).
 */
#include "chain_internal.h"

/* The A:B clock: the playing clip's loop pass, pushed by the shim beside
 * chain_set_clip_phase every frame. A NEW export rather than an argument on
 * that seam: its signature is final (a dlsym'd callee reading a parameter the
 * caller never passed is the breakbeat header-drift crash).
 *
 * RT: SPI callback. Store only. */
__attribute__((visibility("default")))
void chain_set_clip_pass(void *instance, long pass)
{
    chain_instance_t *inst = (chain_instance_t *)instance;
    if (inst) inst->chance_pass1 = (pass >= 0) ? pass + 1 : 0;
}

int chance_filter(chain_instance_t *inst, const uint8_t *msg, int len, int source)
{
    /* Move's notes arrive as EXTERNAL (the cable-2 MIDI_OUT echo). Host clock,
     * FX broadcast and touch are not notes to roll. In Pre mode (Schw+Move)
     * v2_on_midi calls this AFTER the echo filter -- see chain_midi.c. */
    if (!inst || source != MOVE_MIDI_SOURCE_EXTERNAL || len < 3) return 1;
    const uint8_t type = msg[0] & 0xF0;
    if (type != 0x90 && type != 0x80) return 1;
    return sc_gate(&inst->chance_gate, &inst->chance, msg, len,
                   inst->clip_phase_valid, inst->clip_phase_beats,
                   inst->clip_loop_start, inst->clip_loop_len,
                   inst->lane_clip_slot, inst->chance_pass1 - 1);
}

/* MOVE'S EDITS, as the lanes hear them (host/edit_follow.h issues each only
 * once the live model confirms Move made it). Seen here BEFORE the lanes, and
 * independent of them: lanes_off turns automation off, not chance. */
void chance_on_lane_verb(chain_instance_t *inst, const char *sub, const char *val)
{
    if (!inst || !sub || !val) return;
    if (strcmp(sub, "paste_span") == 0) {
        /* "track slot src dst len jid[ v=36,38]" -- a step/page paste, or
         * Double Loop (the loop onto its new half). v= names the pitches Move
         * actually pasted: a drum paste moves one pad's conditions. */
        int track, slot, used = 0; double src, dst, len; unsigned jid;
        if (sscanf(val, "%d %d %lf %lf %lf %u%n", &track, &slot, &src, &dst, &len, &jid, &used) != 6 ||
            slot < 0 || slot >= SC_ROW_PARK || !(len > 0.0))
            return;
        static uint8_t pitches[128];
        const char *v = strstr(val + used, "v=");
        int scoped = 0;
        if (v) {
            memset(pitches, 0, sizeof pitches);
            const char *p = v + 2;
            for (;;) {
                int n = 0, k = 0;
                if (sscanf(p, "%d%n", &n, &k) != 1) break;
                if (n >= 0 && n < 128) { pitches[n] = 1; scoped = 1; }
                p += k;
                if (*p != ',') break;
                p++;
            }
        }
        sc_journal_t *j = &inst->chance_journal[jid % SC_JOURNAL];
        j->jid = jid;
        sc_store_paste(&inst->chance, slot, src, dst, len, scoped ? pitches : NULL, j);
    } else if (strcmp(sub, "journal") == 0) {
        /* "undo|redo jid" -- Move reverted (or re-did) that paste. The high
         * bit is Schwung's own automation edits, never a chance event. */
        char dir[8] = { 0 }; unsigned jid = 0;
        if (sscanf(val, "%7s %u", dir, &jid) != 2 || !jid || (jid & 0x80000000u)) return;
        sc_journal_t *j = &inst->chance_journal[jid % SC_JOURNAL];
        if (j->jid == jid) sc_journal_apply(&inst->chance, j, strcmp(dir, "redo") == 0);
    } else if (strcmp(sub, "stash") == 0) {
        /* "track slot sid" -- the clip was deleted: park its conditions. */
        int track, slot; unsigned sid;
        if (sscanf(val, "%d %d %u", &track, &slot, &sid) != 3 || slot < 0 || slot >= SC_ROW_PARK) return;
        inst->chance_stash_sid[sid % SC_STASHES] = sid;
        sc_store_move_row(&inst->chance, slot, SC_ROW_STASH + (int)(sid % SC_STASHES));
    } else if (strcmp(sub, "unstash") == 0) {
        /* "sid track slot" -- Move's Undo brought the very clip back. */
        unsigned sid; int track, slot;
        if (sscanf(val, "%u %d %d", &sid, &track, &slot) != 3 || slot < 0 || slot >= SC_ROW_PARK) return;
        if (inst->chance_stash_sid[sid % SC_STASHES] != sid) return;   /* overwritten */
        inst->chance_stash_sid[sid % SC_STASHES] = 0;
        sc_store_move_row(&inst->chance, SC_ROW_STASH + (int)(sid % SC_STASHES), slot);
    } else if (strcmp(sub, "copy_clip") == 0) {
        /* "src dst" -- Move copied a clip to another slot on the track. */
        int from, to;
        if (sscanf(val, "%d %d", &from, &to) != 2 || from < 0 || to < 0 ||
            from >= SC_ROW_PARK || to >= SC_ROW_PARK) return;
        sc_store_copy_row(&inst->chance, from, to);
    }
}

/* "row cond [g=<step>] id pitch start [id pitch start ...]" -- one condition
 * for every note on a step, which is ONE trig: `g=` is the step's position,
 * and the notes roll together (a chord played in live starts a few ms apart
 * per note). Without it each note is its own group. Returns how many stored. */
static int chance_set_notes(chain_instance_t *inst, const char *val)
{
    int row, cond, used = 0;
    if (sscanf(val, "%d %d%n", &row, &cond, &used) != 2) return -1;
    if (!sc_valid(cond)) return -1;
    const char *p = val + used;
    double grp = NAN;
    {
        int k = 0;
        if (sscanf(p, " g=%lf%n", &grp, &k) == 1) p += k; else grp = NAN;
    }
    int n = 0;
    for (;;) {
        long long id; int pitch; double start; int k = 0;
        if (sscanf(p, " %lld %d %lf%n", &id, &pitch, &start, &k) != 3) break;
        if (sc_store_set_grp(&inst->chance, row, (int64_t)id, pitch, start, cond,
                             isnan(grp) ? start : grp)) n++;
        p += k;
    }
    return n;
}

void chance_param_set(chain_instance_t *inst, const char *sub, const char *val)
{
    if (!inst || !sub) return;
    if (!val) val = "";
    if (strcmp(sub, "notes") == 0) {
        chance_set_notes(inst, val);
    } else if (strcmp(sub, "move") == 0) {
        int row, pitch; long long id; double start;
        if (sscanf(val, "%d %lld %d %lf", &row, &id, &pitch, &start) == 4)
            sc_store_relocate(&inst->chance, row, (int64_t)id, pitch, start);
    } else if (strcmp(sub, "prune") == 0) {
        /* "row lo hi [id ...]": entries on `row` starting in [lo, hi) whose
         * note Move no longer has. The shim sends the notes it can SEE. */
        int row, used = 0; double lo, hi;
        if (sscanf(val, "%d %lf %lf%n", &row, &lo, &hi, &used) != 3) return;
        int64_t live[128]; int n = 0; const char *p = val + used;
        for (;;) {
            long long id; int k = 0;
            if (n >= 128 || sscanf(p, " %lld%n", &id, &k) != 1) break;
            live[n++] = (int64_t)id; p += k;
        }
        if (n < 128) sc_store_prune_window(&inst->chance, row, lo, hi, live, n);
    } else if (strcmp(sub, "adopt") == 0) {
        /* "row id pitch start": a copied condition becomes Move's note's. */
        int row, pitch; long long id; double start;
        if (sscanf(val, "%d %lld %d %lf", &row, &id, &pitch, &start) != 4) return;
        sc_store_adopt(&inst->chance, row, (int64_t)id, pitch, start);
    } else if (strcmp(sub, "state") == 0) {
        /* A refused document leaves the store as it was (sc_store_parse). */
        sc_store_parse(&inst->chance, val);
    } else if (strcmp(sub, "clear") == 0) {
        sc_store_parse(&inst->chance, "");
    }
}

/* -1 for an unknown subkey, so it reads as a FAILED read, never as "". */
int chance_param_get(chain_instance_t *inst, const char *sub, char *buf, int buf_len)
{
    if (!inst || !sub || !buf || buf_len <= 0) return -1;
    if (strcmp(sub, "rev") == 0)
        return snprintf(buf, buf_len, "%u", (unsigned)inst->chance.rev);
    /* "matched dropped": note-ons that carried a condition, and how many of
     * them lost -- how chance is VERIFIED on a device nobody can hear, and
     * how a "chance does nothing" report is told from a routing problem. */
    if (strcmp(sub, "stats") == 0)
        return snprintf(buf, buf_len, "%u %u", (unsigned)inst->chance_gate.matched,
                        (unsigned)inst->chance_gate.dropped_n);
    if (strcmp(sub, "count") == 0)
        return snprintf(buf, buf_len, "%d", sc_store_count(&inst->chance));
    if (strcmp(sub, "state") == 0) {
        /* Served-and-empty is "" -- the autosave deletes the file on it. */
        if (sc_store_count(&inst->chance) == 0) { buf[0] = 0; return 0; }
        return sc_store_serialize(&inst->chance, buf, buf_len);
    }
    /* "of:<row>:<id>" -- the condition on one note, as its jog index. */
    if (strncmp(sub, "of:", 3) == 0) {
        int row; long long id;
        if (sscanf(sub + 3, "%d:%lld", &row, &id) != 2) return -1;
        return snprintf(buf, buf_len, "%d", sc_store_get(&inst->chance, row, (int64_t)id));
    }
    return -1;
}
