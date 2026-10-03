/*
 * slot_load_job.h — which chain-slot writes LOAD a module, and the three
 * states of the one load allowed in flight.
 *
 * Header-only and dependency-free so tests/host can run it, for the same
 * reason fx_load_gate.h is.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS EXISTS. A slot's `synth:module`, `fxN:module`, `midi_fxN:module`,
 * `load_file`, `load_patch` and `clear` writes reach the chain host's
 * set_param from shadow_inprocess_handle_param_request — the SPI callback, at
 * SCHED_FIFO 70 on core 3. The chain host then dlopens, runs the module's
 * create_instance, parses module.json and restores its state, synchronously.
 * Measured on hardware (param-slow): `load_file` at 432 ms and 225 ms, `clear`
 * at 11 ms. For that long NOTHING on the device gets audio — not the other
 * slots, not Move — which is the hiccup heard on every module load.
 *
 * Master FX and send FX positions were already moved off the callback
 * (fx_load_gate.h), and bus FX before them (chain_bus.c). The chain SLOTS were
 * the path left behind.
 *
 * THE SHAPE. A one-module write is staged (see the two kinds below). A whole
 * patch — a synth, eight audio FX, eight MIDI FX, buses and scenes in one go —
 * is not: staging all of that would mean rebuilding the chain host's patch
 * loader. For those, the whole chain INSTANCE is handed to a loader thread:
 *
 *   RT      takes the slot's instance pointer OUT of shadow_chain_slots[]
 *           (the slot is "parked": every chain host entry point already
 *           answers a NULL instance with silence / -1 / nothing), stores the
 *           request, and wakes the loader. It does NOT answer the param
 *           request — the channel stays held, exactly as it was while the
 *           callback blocked, so the client's wait semantics are unchanged.
 *   LOADER  calls set_param on the parked instance. It owns the instance
 *           outright: nothing on the RT side can reach it.
 *   RT      sees DONE, puts the pointer back, runs the same activation the
 *           synchronous path ran, and only then publishes the response.
 *
 * ONE job, not a queue: the param channel carries one request at a time and
 * the request IS held for the whole load, so a second one cannot arrive.
 */

#ifndef SLOT_LOAD_JOB_H
#define SLOT_LOAD_JOB_H

#include <string.h>

#define SLOT_LOAD_IDLE     0   /* nothing in flight; RT may post */
#define SLOT_LOAD_WORKING  1   /* loader-owned (PARK: the instance; STAGED: the stage) */
#define SLOT_LOAD_DONE     2   /* loader finished; RT reinstalls / swaps, then answers */
#define SLOT_LOAD_FADING   3   /* PARK only, RT-owned: slot fading out before the park */
#define SLOT_LOAD_RETIRING 4   /* STAGED only: answered; loader destroys the old module */

/*
 * TWO KINDS OF JOB.
 *
 * STAGED (`synth:module`, `fxN:module`, `midi_fxN:module` — ONE module) is
 * never parked. The new module is built on the loader while the callback keeps
 * rendering the slot — every module, Move's track — untouched; then the
 * callback fades ONLY the outgoing module (the synth's output, or that one FX
 * position wet -> dry; a MIDI FX has no audio) and swaps it, and a new FX fades
 * in from dry (chain_synth_load.c, chain_fx_load.c); then the loader destroys
 * the old one. Nothing else in the slot ever leaves the signal.
 *
 * PARK (`load_file`, `load_patch`, `patch`, `clear`) replaces the whole patch,
 * so the instance is handed over whole: the slot is FADED OUT first (its own
 * 50 ms slot fade) and faded back in after.
 */
#define SLOT_LOAD_KIND_PARK   0
#define SLOT_LOAD_KIND_STAGED 1

static inline int slot_load_key_is_synth_swap(const char *key)
{
    return key && strcmp(key, "synth:module") == 0;
}

static int slot_load_key_is_async(const char *key);

/* One module, staged: synth:module and the fx / midi_fx position writes. */
static inline int slot_load_key_is_staged(const char *key)
{
    if (!key) return 0;
    if (slot_load_key_is_synth_swap(key)) return 1;
    return slot_load_key_is_async(key) &&
           (strncmp(key, "fx", 2) == 0 || strncmp(key, "midi_fx", 7) == 0);
}

/* Blocks a PARK waits for its fade-out before parking anyway: a slot the
 * shim's idle gate is not rendering never advances its fade. 50 ms of fade is
 * ~17 blocks; this is ~70 ms. */
#define SLOT_LOAD_FADE_MAX_BLOCKS 24

/*
 * Does this slot-level key load or unload modules?
 *
 * Deliberately a short, exact list. A knob write must NEVER park a slot — that
 * would cut its audio for a frame on every detent — so this matches whole
 * keys, never prefixes, and `fx1:module_x` or `synth:modules` are not loads.
 */
static inline int slot_load_key_is_async(const char *key)
{
    if (!key) return 0;
    if (strcmp(key, "synth:module") == 0) return 1;
    if (strcmp(key, "load_file") == 0)    return 1;
    if (strcmp(key, "load_patch") == 0)   return 1;
    if (strcmp(key, "patch") == 0)        return 1;
    if (strcmp(key, "clear") == 0)        return 1;

    /* fx<N>:module / midi_fx<N>:module — the same grammar shadow_fx_key.h
     * accepts: at least one digit, no leading zero. */
    const char *p = key;
    if (strncmp(p, "midi_fx", 7) == 0) p += 7;
    else if (strncmp(p, "fx", 2) == 0) p += 2;
    else return 0;
    if (*p < '1' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, ":module") == 0;
}

#endif /* SLOT_LOAD_JOB_H */
