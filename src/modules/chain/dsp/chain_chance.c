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
     * FX broadcast and touch are not notes to roll. Pre mode: the MIDI FX
     * inject into Move and its echo filter counts what went out -- dropping
     * an echo there would unbalance that count, so chance stands down. */
    if (!inst || source != MOVE_MIDI_SOURCE_EXTERNAL || len < 3) return 1;
    if (inst->midi_fx_pre_mode) return 1;
    const uint8_t type = msg[0] & 0xF0;
    if (type != 0x90 && type != 0x80) return 1;
    return sc_gate(&inst->chance_gate, &inst->chance, msg, len,
                   inst->clip_phase_valid, inst->clip_phase_beats,
                   inst->clip_loop_start, inst->clip_loop_len,
                   inst->lane_clip_slot, inst->chance_pass1 - 1);
}

/* "row cond id pitch start [id pitch start ...]" -- one condition for every
 * note on a step (a chord shares its trig). Returns how many were stored. */
static int chance_set_notes(chain_instance_t *inst, const char *val)
{
    int row, cond, used = 0;
    if (sscanf(val, "%d %d%n", &row, &cond, &used) != 2) return -1;
    if (!sc_valid(cond)) return -1;
    const char *p = val + used;
    int n = 0;
    for (;;) {
        long long id; int pitch; double start; int k = 0;
        if (sscanf(p, " %lld %d %lf%n", &id, &pitch, &start, &k) != 3) break;
        if (sc_store_set(&inst->chance, row, (int64_t)id, pitch, start, cond)) n++;
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
