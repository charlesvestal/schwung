/*
 * chain_internal.h — shared types and cross-TU declarations for the Signal
 * Chain DSP plugin (split out of chain_host.c, 2026-06 cleanup step 10).
 *
 * Everything marked CHAIN_INTERNAL is hidden-visibility: dsp.so's exported
 * symbol surface must stay exactly the 5 public entry points (see
 * chain_host.c) plus unified_log* — sub-plugins are dlopen'd and must never
 * be able to bind against these internals.
 */
#ifndef CHAIN_INTERNAL_H
#define CHAIN_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <malloc.h>

/* Sentinel for "channel field not present in patch file".
 * Distinguishes genuine absence from the legal 0 values used by
 * receive_channel (0=All) and forward_channel (0=ch 1 internal). */
#define PATCH_CHANNEL_UNSET INT_MIN

/* Lives in src/host/ rather than beside this file: the SHIM needs it too
 * (Master FX is a permuted list now), and there is no include path from
 * src/host/ into a dlopen'd module's private directory. Nothing about it is
 * chain-specific, and a second copy is precisely the bug class that made
 * fx3..fx8 silent. Reached the same way as plugin_api_v1.h below. */
#include "host/chain_key_index.h"
#include "host/json_compact.h"
#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/midi_fx_api_v1.h"
#include "host/lfo_common.h"
#include "host/bus_mix.h"
#include "host/bus_route.h"
#include "../../../host/unified_log.h"
#include "../../../host/shadow_constants.h"

/* Limits */
#define MAX_PATCHES 32      /* Max patches to list in browser */
#define MAX_AUDIO_FX 8      /* Max FX loaded per active chain */

/* Buses a slot can hold, BESIDE Main. Main is bus 0 and is implicit: it is
 * never created or deleted, holds every voice not assigned elsewhere, and its
 * insert chain IS the slot's existing main chain. So a slot holds up to
 * SLOT_BUSES + 1 mixing destinations and (SLOT_BUSES + 1) * MAX_AUDIO_FX
 * positions.
 *
 * Raising this should be a one-line change: all "bus<N>:" key routing goes
 * through bus_route.h with this passed in as bus_count, and every loop over
 * buses is bounded by this name. Read out of this line by
 * tests/host/test_bus_route.sh. The bitmask in bus_mix_active_mask is a
 * uint32_t, so 32 is the hard ceiling. */
#define SLOT_BUSES 4
/* Named, not a repeated 32: bus_mix.h is included above now (the render path
 * needs it), so the copy this header used to carry has no excuse left. */
_Static_assert(SLOT_BUSES > 0 && SLOT_BUSES <= BUS_MIX_MAX_BUSES,
               "SLOT_BUSES must fit bus_mix_active_mask's uint32_t");

#define MAX_MIDI_FX 8       /* Max native MIDI FX modules per chain */
#define CHAIN_PRE_DELAY_MAX 32  /* Pre-mode inject-delay buffer: one clock's output */
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64

/* Voices a module can declare, and how long an id may be. Hoisted this far up
 * because BOTH bus_config_t (the patch-file form) and slot_bus_t (the runtime
 * form) store the ids of the voices assigned to a bus. They describe
 * chain_instance_t::synth_split_voice_ids, which is where the meaning of the
 * index is documented. */
#define SPLIT_VOICES_MAX 32
#define SPLIT_VOICE_ID_LEN 32

/* Optional file-based debug tracing for chain parsing/preset save diagnostics. */
#define CHAIN_DEBUG_FLAG_PATH "/data/UserData/schwung/chain_debug_on"
#define CHAIN_DEBUG_LOG_PATH "/data/UserData/schwung/chain_debug.log"
#define MOVE_SETTINGS_JSON_PATH "/data/UserData/settings/Settings.json"
#define CLOCK_SETTINGS_MAX_BYTES (256 * 1024)
#define CLOCK_SETTINGS_REFRESH_MS 1000
#define CLOCK_TICK_STALE_MS 750

/* MIDI input filter */
typedef enum {
    MIDI_INPUT_ANY = 0,
    MIDI_INPUT_PADS,
    MIDI_INPUT_EXTERNAL
} midi_input_t;

#define SAMPLE_RATE 44100
#define FRAMES_PER_BLOCK 128
#define MOVE_STEP_NOTE_MIN 16
#define MOVE_STEP_NOTE_MAX 31
#define MOVE_PAD_NOTE_MIN 68

/* Knob mapping constants */
#define MAX_KNOB_MAPPINGS 8
#define KNOB_CC_START 71
#define KNOB_CC_END 78
#define KNOB_ABS_CC_START 102
#define KNOB_ABS_CC_END 109
#define KNOB_STEP_FLOAT 0.0015f /* Base step for floats (~600 clicks for 0-1 at min speed) */
#define KNOB_STEP_INT 1        /* Base step for int params */

/* Knob acceleration settings */
#define KNOB_ACCEL_MIN_MULT 1    /* Multiplier for slow turns */
#define KNOB_ACCEL_MAX_MULT 4    /* Multiplier for fast turns (floats) */
#define KNOB_ACCEL_MAX_MULT_INT 2 /* Multiplier for fast turns (ints) */
#define KNOB_ACCEL_ENUM_MULT 1   /* Enums: always step by 1 (no acceleration) */
#define KNOB_ACCEL_SLOW_MS 250   /* Slower than this = min multiplier */
#define KNOB_ACCEL_FAST_MS 50    /* Faster than this = max multiplier */

/* Knob mapping types */
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1,
    KNOB_TYPE_ENUM = 2
} knob_type_t;

/* Knob mapping structure */
typedef struct {
    int cc;              /* CC number (71-78 for knobs 1-8) */
    char target[16];     /* Component: "synth", "fx1", "fx2", "midi_fx" */
    char param[32];      /* Parameter key (lookup metadata in chain_params) */
    float current_value; /* Current value only */
    int last_cc_out;     /* Last 0-127 value emitted to the external port,
                          * -1 = nothing sent yet. Change detection happens at
                          * CC resolution, so a knob swept through a range that
                          * maps to one CC step emits once, not once per turn. */
} knob_mapping_t;

/* Chain parameter info from module.json */
#define MAX_CHAIN_PARAMS 256
#define MAX_ENUM_OPTIONS 128
/* Cached ui_hierarchy JSON, per position. Named because the buffers it sizes
 * are now reached through a pointer, where sizeof() would answer 8. */
#define CHAIN_UI_HIERARCHY_LEN 65536
typedef struct {
    char key[32];           /* Parameter key (e.g., "preset", "decay") */
    char name[64];          /* Display name */
    knob_type_t type;       /* Parameter type: FLOAT, INT, or ENUM */
    float min_val;          /* Minimum value */
    float max_val;          /* Maximum value (or -1 if dynamic via max_param) */
    float default_val;      /* Default value */
    char max_param[32];     /* Dynamic max param key (e.g., "preset_count") */
    char unit[16];          /* Unit suffix (e.g., "Hz", "dB", "%") */
    char display_format[16]; /* Display format hint (e.g., "%.2f", "%d") */
    float step;             /* Step size for UI increments */
    char options[MAX_ENUM_OPTIONS][32];  /* Enum options (if type is ENUM) */
    int option_count;       /* Number of enum options */
} chain_param_info_t;

#define MAX_MOD_TARGETS 32
#define MAX_MOD_SOURCES_PER_TARGET 8
#define MOD_PARAM_CACHE_REFRESH_MS 250
#define MOD_FLOAT_CHANGE_EPSILON 0.000001f
#define MOD_INT_ENUM_MIN_INTERVAL_MS 50

typedef struct mod_source_contribution {
    int active;
    char source_id[32];
    float contribution;
} mod_source_contribution_t;

/* Runtime modulation target state (non-destructive overlay). */
typedef struct mod_target_state {
    int active;
    int enabled;
    char target[16];
    char param[32];
    float base_value;
    mod_source_contribution_t sources[MAX_MOD_SOURCES_PER_TARGET];
    float effective_value;
    float last_applied_value;
    uint64_t last_applied_ms;
    int has_last_applied;
    float min_val;
    float max_val;
    knob_type_t type;
} mod_target_state_t;

#define MOVE_PAD_NOTE_MAX 99

/* MIDI FX parameter storage (key-value pairs for flexible configuration) */
#define MAX_MIDI_FX_PARAMS 8
typedef struct {
    char key[32];
    char val[32];
} midi_fx_param_t;

/* State storage size for FX plugins */
#define MAX_FX_STATE_LEN 8192

/*
 * State storage for a BUS FX position, deliberately an eighth of the above.
 *
 * A patch_info_t is a STACK local in v2_set_param's "load_file" route — i.e.
 * on the SPI callback's stack — and already ~160 KB. Buses add
 * SLOT_BUSES * MAX_AUDIO_FX more state buffers, which at MAX_FX_STATE_LEN
 * would be another 280 KB on that stack. 1 KB keeps the addition to ~35 KB.
 *
 * It is not a truncation: v2_parse_patch_file drops a state that does not fit
 * (json_object_compact_copy answers -1 and the field is left empty), so an
 * over-long bus FX state comes back at the plugin's defaults rather than as a
 * half-parsed string. The whole patch file is capped at 65536 bytes anyway,
 * so a full-size state per bus position could never have been stored.
 */
#define MAX_BUS_FX_STATE_LEN 1024


/* MIDI FX configuration (module + params + state) */
typedef struct {
    char module[MAX_NAME_LEN];
    midi_fx_param_t params[MAX_MIDI_FX_PARAMS];
    int param_count;
    char state[MAX_FX_STATE_LEN];  /* JSON state for MIDI FX plugin */
} midi_fx_config_t;

/* Audio FX configuration (module + params + state) */
typedef struct {
    char module[MAX_NAME_LEN];
    midi_fx_param_t params[MAX_MIDI_FX_PARAMS];  /* Reuse param struct */
    int param_count;
    char state[MAX_FX_STATE_LEN];  /* JSON state for audio FX plugin */
} audio_fx_config_t;

/* One bus FX position as it is stored in a patch file. */
typedef struct {
    char module[MAX_NAME_LEN];
    int  bypassed;
    char state[MAX_BUS_FX_STATE_LEN];
} bus_fx_config_t;

/*
 * One bus as it is stored in a patch file.
 *
 * `present` is not redundant with a name or an FX count: an EMPTY bus that the
 * user created is a different thing from a bus the file never mentioned, and
 * only the first should be re-created (and re-allocated) on load.
 *
 * Voices are stored as IDS. See bus_voice_apply.h for why, and for what
 * happens to one that no longer resolves.
 */
typedef struct {
    int  present;
    char name[MAX_NAME_LEN];
    char voice_ids[SPLIT_VOICES_MAX][SPLIT_VOICE_ID_LEN];
    int  voice_id_count;
    int  sends[BUS_MIX_SENDS];
    bus_fx_config_t fx[MAX_AUDIO_FX];
    int  fx_count;
} bus_config_t;

/* Synth state storage size - Surge XT needs ~8KB+ when pretty-printed with indent */
#define MAX_SYNTH_STATE_LEN 16384

/* LFO types, shapes, divisions, and waveform computation from lfo_common.h */

/* Patch info */
typedef struct {
    char name[MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char synth_module[MAX_NAME_LEN];
    int synth_preset;
    char synth_state[MAX_SYNTH_STATE_LEN];  /* JSON state for synth plugin */
    char midi_source_module[MAX_NAME_LEN];
    audio_fx_config_t audio_fx[MAX_AUDIO_FX];  /* Now includes params */
    int audio_fx_count;
    midi_fx_config_t midi_fx[MAX_MIDI_FX];      /* Native MIDI FX with params */
    int midi_fx_count;
    midi_input_t midi_input;
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    int receive_channel;   /* PATCH_CHANNEL_UNSET=absent, 0=All, 1-16=specific channel */
    int forward_channel;   /* PATCH_CHANNEL_UNSET=absent, -2=passthrough, -1=auto, 0-15=channel */
    int midi_fx_pre_mode;  /* 0 = Post (default), 1 = Pre (additive inject to Move MIDI_IN) */
    int knob_cc_out;       /* 0 = off (default), 1 = echo chain-knob changes out
                            * as CC 102-109 on the slot's recv channel */
    lfo_state_t lfos[LFO_COUNT];  /* LFO configuration */
    bus_config_t buses[SLOT_BUSES];
    int main_sends[BUS_MIX_SENDS];
} patch_info_t;

/* ============================================================================
 * Parameter Smoothing (to avoid zipper noise on knob changes)
 * ============================================================================ */

#define MAX_SMOOTH_PARAMS 16
#define SMOOTH_COEFF 0.15f  /* Smoothing coefficient per block (~5ms at 128 frames/44100Hz) */

typedef struct {
    char key[MAX_NAME_LEN];
    float target;
    float current;
    int active;
} smooth_param_t;

typedef struct {
    smooth_param_t params[MAX_SMOOTH_PARAMS];
    int count;
} param_smoother_t;

/* LFO engine: shapes, divisions, and waveform computation now in lfo_common.h */

/* ============================================================================
 * V2 Instance-Based API
 * ============================================================================ */

/* Capacity of a bus buffer, in int16_t samples (stereo interleaved). This is
 * the ONE name both sides of the allocation gap must read: the render path
 * below sizes every memset/memcpy through bus buffers off this macro, and
 * Task 5's allocator (not yet written — see the TODO on `buf` below) MUST
 * allocate exactly this many samples per bus. Changing FRAMES_PER_BLOCK
 * changes this too, automatically, so the two can never drift apart the way
 * a repeated comment could. */
#define BUS_BUF_SAMPLES (FRAMES_PER_BLOCK * 2)

/*
 * One of a slot's SLOT_BUSES sub-mixes: a buffer the synth renders a subset of
 * its voices into, plus that bus's own insert chain.
 *
 * `buf` is allocated ON DEMAND (off the RT thread) and is NULL until then, so
 * a NULL buffer is the normal resting state and not an error — bus_mix_target
 * routes the bus's voices to the main buffer while it is NULL, which is why a
 * bus can be configured before it is allocated without ever dropping audio.
 */
typedef struct {
    /* RT-thread bookkeeping ONLY. The render path's "does this bus exist"
     * test is `buf != NULL`, not this — buf is the one that fails safe, since
     * a bus awaiting its buffer routes through Main. Do not start branching on
     * in_use in the render path: it is written without a release and would
     * become a second, unsynchronised cross-thread signal. */
    int   in_use;
    char  name[MAX_NAME_LEN];
    /* BUS_BUF_SAMPLES int16_t's (stereo interleaved) when non-NULL. The
     * allocator (Task 5, not yet written) MUST size this buffer with the
     * BUS_BUF_SAMPLES macro above, not a repeated literal or a voice-count-
     * derived size — the render path in v2_render_block sizes every
     * memset/memcpy through it against that same name, on the SPI callback,
     * with no bounds check of its own. A mismatch is a silent heap overflow
     * on the realtime thread.
     *
     * Allocated by chain_bus_worker_fn (chain_host.c) and published here with
     * an __ATOMIC_RELEASE store; v2_render_block's snapshot loop reads it with
     * a matching __ATOMIC_ACQUIRE load. Freed in chain_bus_release_all, after
     * the worker has been joined. */
    int16_t *buf;
    void *fx_handles[MAX_AUDIO_FX];
    audio_fx_api_v2_t *fx_plugins_v2[MAX_AUDIO_FX];
    void *fx_instances[MAX_AUDIO_FX];
    /* RT-OWNED. The worker never writes these, so the render path and
     * get_param read them with no gate at all. */
    int   fx_bypassed[MAX_AUDIO_FX];

    /* WORKER-OWNED, PUBLISHED UNDER fx_ready. Everything from here to
     * current_fx_modules is written by chain_bus_worker_fn and must only be
     * read after an ACQUIRE load of fx_ready returns non-zero — see the gate's
     * own comment below. */
    int   fx_count;
    char  current_fx_modules[MAX_AUDIO_FX][MAX_NAME_LEN];
    /*
     * Per-position metadata, allocated PER OCCUPIED POSITION and only by the
     * worker.
     *
     * Not eagerly for all MAX_AUDIO_FX positions the way the main chain's are
     * (chain_alloc_position_storage): one chain_param_info_t table is ~1.1 MB
     * and one ui_hierarchy cache 64 KB, so eager allocation would cost
     * ~9.1 MB per bus, ~36 MB per slot and ~145 MB across four slots — for
     * positions that are almost always empty. A bus therefore costs metadata
     * only for the FX it actually holds.
     */
    chain_param_info_t *fx_params[MAX_AUDIO_FX];
    int   fx_param_counts[MAX_AUDIO_FX];
    char *fx_ui_hierarchy[MAX_AUDIO_FX];          /* CHAIN_UI_HIERARCHY_LEN each */
    /*
     * THE SECOND GATE, and it is not optional.
     *
     * `buf`'s RELEASE/ACQUIRE pair covers `buf` AND NOTHING ELSE: the fields
     * above are written by the worker AFTER buf is published, so buf's acquire
     * cannot order them. Non-zero means "the worker is done and these fields
     * are stable"; the render path and every get_param that touches an FX
     * instance load it __ATOMIC_ACQUIRE first and skip the bus's inserts
     * entirely when it reads 0.
     *
     * Cleared by the RT thread BEFORE it hands work to the worker. That store
     * needs no release of its own: set_param and render_block are the SAME
     * thread (the SPI callback), so once set_param has cleared it, no later
     * render can run these FX and no earlier one is still running.
     */
    int   fx_ready;
    /*
     * Bumped by the RT thread before every post, read by the worker.
     *
     * The worker publishes fx_ready only when the seq it started from still
     * matches — otherwise a request the RT thread made mid-reconcile would be
     * published as finished. On a mismatch it leaves fx_ready at 0 and runs
     * again on the post that accompanied the bump, so it converges without the
     * RT side ever waiting.
     */
    unsigned fx_req_seq;

    /* --- RT-OWNED REQUEST SIDE. The SHAPE the user asked for, which is also
     * what get_param and serialization answer from: it is never written by the
     * worker, so reading it needs no gate and cannot tear. --- */
    char  fx_request[MAX_AUDIO_FX][MAX_NAME_LEN];
    /* Opaque plugin state staged for the worker to apply after it creates an
     * instance. Set only by a patch load; a live edit goes straight to the
     * plugin. Consumed (and cleared) by the worker. */
    char  fx_state_request[MAX_AUDIO_FX][MAX_BUS_FX_STATE_LEN];
    int   fx_state_pending[MAX_AUDIO_FX];
    /* Buffer the RT thread has unpublished (stored NULL over `buf`) and handed
     * to the worker to free. Freeing on the RT thread is the alternative and it
     * is a free() on the SPI callback. */
    int16_t *buf_retired;

    /* --- Voice assignment, RT-owned. --- *
     *
     * The IDS are the configuration; chain_instance_t::voice_bus is a derived
     * cache rebuilt from them. An id that does not resolve STAYS HERE — it is
     * counted in orphan_count and left out of the map, never dropped and never
     * re-pointed, so it comes back if the module that declares it does.
     */
    char  voice_ids[SPLIT_VOICES_MAX][SPLIT_VOICE_ID_LEN];
    int   voice_id_count;
    int   orphan_count;

    int   send_level[BUS_MIX_SENDS];              /* 0..BUS_MIX_SEND_LEVEL_MAX */
} slot_bus_t;

/* Chain instance state - contains all per-instance data for v2 API */
typedef struct chain_instance {
    /* Module directory */
    char module_dir[MAX_PATH_LEN];

    /* Sub-plugin state - Synth */
    void *synth_handle;
    plugin_api_v2_t *synth_plugin_v2;
    void *synth_instance;
    char current_synth_module[MAX_NAME_LEN];
    int synth_default_forward_channel;  /* -1 = no default, 0-15 = channel */
    int synth_consumes_line_input;
    /* MIDI note last played INTO the synth, or -1.
     *
     * The fallback answer to "which voice is focused" for a module that
     * declares no focus param of its own. It is a NOTE and not a voice index
     * on purpose: resolving the index needs the canonical voice order, and a
     * second implementation of that order in C -- next to voices.mjs, with
     * chain_json.c's flat key-scan helpers, which cannot walk `levels` in
     * order -- is the metronome / recall_quantize off-by-one shape. It would
     * fail silently as "the grid follows the wrong pad".
     *
     * Written on the SPI callback: a plain int store, nothing else. */
    int synth_last_note;
    int synth_wants_sysex;  /* capabilities.wants_sysex on the synth */      /* 1 = pulls line-in/mic (feedback risk on boot) */

    /* Voices this synth can render into separate buffers, in the module's own
     * declared order — the index here IS the voice_out[] index handed to
     * move_plugin_render_split.
     *
     * FLAT AND ORDERED ON PURPOSE. The bus->voice map has to be resolved in C on
     * the SPI callback, and chain_json.c's helpers are flat key scans that cannot
     * walk ui_hierarchy's `levels` in order — the same constraint that makes
     * synth:last_note report a note rather than a voice index. So the module
     * publishes a flat array and we never try to walk its hierarchy here.
     *
     * Reset on create and on every synth load: an id left over from the previous
     * module must not name a voice in a list that no longer exists. */
    char synth_split_voice_ids[SPLIT_VOICES_MAX][SPLIT_VOICE_ID_LEN];
    int  synth_split_voice_count;

    /* Optional per-voice render, discovered by dlsym on the synth handle.
     *
     * A SEPARATE EXPORTED SYMBOL, NOT A FIELD ON plugin_api_v2_t. Appending to
     * that struct is what boot-looped a device via breakbeat's header drift: a
     * module cannot extend the ABI from its side, and a guarded read of a field
     * we do not have tests memory belonging to somebody else. A dlsym'd symbol
     * is absent-or-present with no offset to get wrong.
     *
     * It ACCUMULATES — the chain clears the buffers first — which is the
     * opposite of render_block, and is what makes two voices sharing one bus
     * cost no mixing pass at all. */
    void (*synth_render_split)(void *instance, int16_t *const *voice_out,
                               int n_voices, int frames);

    /* voice index -> bus index, or BUS_MIX_MAIN. Indexed by the SAME index as
     * synth_split_voice_ids, holes included: a hole never matches a bus
     * assignment and so resolves to main like any unassigned voice.
     *
     * Initialised to BUS_MIX_MAIN, never left at calloc's 0 — 0 is a real bus
     * index and would put every voice on bus 1 the moment buses allocate. */
    int8_t voice_bus[SPLIT_VOICES_MAX];

    /* Per-bus sub-mixes and their insert chains. Main is bus 0 and implicit:
     * it is this instance's own out buffer and its existing fx[] chain. */
    slot_bus_t buses[SLOT_BUSES];
    int main_send_level[BUS_MIX_SENDS];  /* Main sends like any bus */

    /* Which buses v2_render_block actually rendered into on the LAST frame.
     * Written by the render, read by chain_drain_sends, both on the SPI
     * callback, so no synchronisation is involved.
     *
     * It exists because "has a buffer" is not "was rendered this frame": a bus
     * whose last voice was reassigned to Main keeps its allocated buffer, and
     * the render clears only the buses the mask names. Draining on buf != NULL
     * would therefore go on sending that bus's final 128 frames forever — a
     * drone with no note behind it. */
    uint32_t bus_rendered_mask;

    /*
     * Bus allocation is a REQUEST, not an action.
     *
     * create_instance, set_param and every other module entry point run on the
     * SPI callback (SCHED_FIFO, core 3, ~2370 us for the whole device), so the
     * allocation a bus needs cannot happen where it is asked for. Today that is
     * only the 512-byte mix buffer, but the shape is chosen for what a bus will
     * cost once it carries its own per-position metadata: 8 positions of
     * chain_param_info_t (~1.07 MB) plus 8 x 64 KB of cached ui_hierarchy,
     * ~9.1 MB — a multi-megabyte calloc inside the audio thread.
     *
     * So the RT side sets bus_alloc_pending[b], posts the semaphore and
     * returns; the worker (SCHED_OTHER, cores 0-2) allocates and publishes buf
     * by pointer; the RT side sees it appear on a later frame. Until it does,
     * bus_mix_target resolves the bus to NULL and its voices are heard through
     * Main, so nothing is ever dropped waiting for memory.
     */
    /* Accessed with __atomic_* from both threads, so the qualifier buys
     * nothing — volatile orders nothing and implies plain access would do.
     * Written RELEASE / read ACQUIRE at every site instead.
     *
     * bus_worker_started is stored RELEASE but read PLAIN on the RT side:
     * sound only because the RT thread is its sole writer and reads its own
     * stores. That single-writer rule is the whole justification; if a second
     * writer ever appears, both reads need ACQUIRE. */
    int bus_alloc_pending[SLOT_BUSES];
    pthread_t bus_worker;
    /* 1 between pthread_create and the join in chain_bus_worker_stop. Doubles
     * as the worker's run flag: clearing it and posting the semaphore is the
     * whole shutdown protocol. */
    int bus_worker_started;
    /*
     * A SEMAPHORE, not a condvar, and not a poll.
     *
     * The signaller is the SPI callback. A condvar needs its mutex held to
     * signal safely, and that mutex is also held by a SCHED_OTHER worker — a
     * FIFO 70 thread blocking on a lock owned by a SCHED_OTHER one is textbook
     * priority inversion on the audio thread. sem_post takes no lock: an
     * atomic increment and, only when someone is actually parked, a FUTEX_WAKE.
     * It is also what makes the join at teardown prompt (see
     * chain_bus_worker_stop) — a usleep poll loop would make every destroy wait
     * out its period on the callback.
     */
    sem_t bus_worker_sem;
    int bus_worker_sem_ok;   /* sem_init succeeded; guards sem_destroy */

    /* Audio FX state */
    void *fx_handles[MAX_AUDIO_FX];
    audio_fx_api_v2_t *fx_plugins_v2[MAX_AUDIO_FX];
    void *fx_instances[MAX_AUDIO_FX];
    int fx_is_v2[MAX_AUDIO_FX];
    int fx_count;
    char current_fx_modules[MAX_AUDIO_FX][MAX_NAME_LEN];  /* Track loaded FX names */

    /* Optional MIDI handler for audio FX (discovered via dlsym) */
    void (*fx_on_midi[MAX_AUDIO_FX])(void *instance, const uint8_t *msg, int len, int source);

    /* Module parameter info */
    chain_param_info_t synth_params[MAX_CHAIN_PARAMS];
    int synth_param_count;
    /*
     * POINTERS, not inline arrays, and the reason is the reorder.
     *
     * These two are by far the largest per-position things a chain holds — one
     * `chain_param_info_t` is ~4 KB (it carries 128 enum option strings), so a
     * position's metadata is ~1.1 MB and its cached ui_hierarchy another 64 KB.
     * Moving a module from position N to N+1 has to move its metadata with it,
     * and inline that is a multi-megabyte memmove inside the SPI audio callback
     * (~900 µs budget). Through a pointer it is 8 bytes.
     *
     * Allocated EAGERLY for every position at create_instance and freed at
     * destroy, so they are never NULL and the ~19 MB an instance costs is
     * exactly what it cost when these were inline — nothing about lifetime or
     * footprint changed, only that the elements can now be permuted.
     */
    chain_param_info_t *fx_params[MAX_AUDIO_FX];
    int fx_param_counts[MAX_AUDIO_FX];
    char *fx_ui_hierarchy[MAX_AUDIO_FX];  /* CHAIN_UI_HIERARCHY_LEN each */

    /* Patch state */
    patch_info_t patches[MAX_PATCHES];
    int patch_count;
    int current_patch;

    /* MIDI FX module state */
    void *midi_fx_handles[MAX_MIDI_FX];
    midi_fx_api_v1_t *midi_fx_plugins[MAX_MIDI_FX];
    void *midi_fx_instances[MAX_MIDI_FX];
    int midi_fx_count;
    char current_midi_fx_modules[MAX_MIDI_FX][MAX_NAME_LEN];
    /* Pointers for the same reason as fx_params / fx_ui_hierarchy above. */
    chain_param_info_t *midi_fx_params[MAX_MIDI_FX];
    int midi_fx_param_counts[MAX_MIDI_FX];
    char *midi_fx_ui_hierarchy[MAX_MIDI_FX];  /* CHAIN_UI_HIERARCHY_LEN each */

    /* Knob mapping state */
    knob_mapping_t knob_mappings[MAX_KNOB_MAPPINGS];
    int knob_mapping_count;
    uint64_t knob_last_time_ms[MAX_KNOB_MAPPINGS];  /* For acceleration */

    /* Runtime modulation bus state */
    mod_target_state_t mod_targets[MAX_MOD_TARGETS];
    int mod_target_count;
    uint64_t mod_param_refresh_ms_synth;
    uint64_t mod_param_refresh_ms_fx[MAX_AUDIO_FX];
    uint64_t mod_param_refresh_ms_midi_fx[MAX_MIDI_FX];

    /* Per-slot LFO state */
    lfo_state_t lfos[LFO_COUNT];
    float lfo_base_values[LFO_COUNT];  /* Base value snapshot for LFO-to-LFO modulation */
    int lfo_base_valid[LFO_COUNT];     /* Whether base has been snapshotted */

    /* MIDI input filter */
    midi_input_t midi_input;

    /* Host APIs for sub-plugins */
    host_api_v1_t subplugin_host_api;

    /* Reference to host API (shared) */
    const host_api_v1_t *host;

    /* Parameter smoothing for synth and FX */
    param_smoother_t synth_smoother;
    param_smoother_t fx_smoothers[MAX_AUDIO_FX];

    /* Dirty flag: 1 = modified since last load/save */
    int dirty;

    /* External audio injection (e.g. Move track audio from Link Audio).
     * Set by host before render_block; mixed after synth, before FX. */
    int16_t *inject_audio;
    int inject_audio_frames;

    /* When set, render_block outputs raw synth only (no inject mix, no FX).
     * The shim calls chain_process_fx() separately for same-frame FX. */
    int external_fx_mode;

    /* Channel settings from last load_file (autosave restore).
     * Used as fallback when current_patch == -1 (file-based load, not library). */
    int loaded_receive_channel;   /* PATCH_CHANNEL_UNSET=absent, 0=All, 1-16=specific */
    int loaded_forward_channel;   /* PATCH_CHANNEL_UNSET=absent, -2=passthrough, -1=auto, 0-15=channel */

    /* MIDI FX placement: 0 = Post (default, output goes to slot synth only),
     * 1 = Pre (output also injected into Move's MIDI_IN cable 0 so Move's
     * native instrument on the slot's forward_channel plays it additively).
     * Only meaningful when a MIDI FX is loaded. */
    int midi_fx_pre_mode;
    int knob_cc_out;

    /* Cached "pre_capable" hint from the loaded MIDI FX module.json.
     * Informs the Shadow UI default on first placement; does not gate the
     * per-slot toggle (legacy FX can still be switched to Pre manually). */
    int midi_fx_pre_capable[MAX_MIDI_FX];
    /* Opt-in: this component wants raw SysEx fragments delivered to its
     * on_midi/process_midi. Off by default and per POSITION, so it permutes
     * with everything else in chain_reorder.c. */
     int midi_fx_wants_sysex[MAX_MIDI_FX];

    /* Pre-mode echo refcount: per-note counter tracking notes we injected
     * into Move's MIDI_IN cable 2. Move plays the injection and echoes it
     * back on MIDI_OUT cable 2, which the shim routes to slot chains — we
     * must drop those echoes before they re-enter MIDI FX processing or
     * the chain would transform and re-inject them (feedback loop). The
     * per-note refcount survives chord overlaps; note-off echoes decrement
     * so later note-ons on the same pitch aren't falsely filtered. */
    uint8_t pre_injected_notes[128];

    /* Pre-mode pad-held tracker: counts how many times each note is
     * currently held by a pad via cable-2 MIDI_OUT from Move. Tick-path
     * MIDI FX (arp) must NOT inject a note that's held by a pad, because
     * that would leave our refcount > 0 for the pad's pitch and the real
     * pad-release note-off would get mistaken for an injection echo and
     * eaten (symptom: arp keeps running after pad release). The set is
     * maintained in v2_on_midi after the echo filter so only real pad
     * events — not our own injection echoes — affect it. */
    uint8_t pre_pad_held[128];

    /* Pre-mode inject-only record-align. Clock-driven generator output
     * (Beat Bank etc.) must reach Move's track AFTER the 0xF8 that advances
     * its step, or Move records it one 16th early. We can't delay the note
     * stream itself (the slot synth needs it immediately for tight local
     * timing), so we delay ONLY the inject: this holds one clock's worth of
     * injected messages and flushes them on the next clock/transport message
     * (1-clock delay). Stop (0xFC) flushes immediately so note-offs never
     * strand on Move's track. Only used for 1-byte clock-driven output. */
    uint8_t pre_delay_msg[CHAIN_PRE_DELAY_MAX][3];
    int     pre_delay_len[CHAIN_PRE_DELAY_MAX];
    int     pre_delay_count;
    int     pre_delay_recv_ch;

    /* Per-component bypass flags. 1 = bypassed (skip processing), 0 = active. */
    int synth_bypassed;
    int midi_fx_bypassed[MAX_MIDI_FX];
    int fx_bypassed[MAX_AUDIO_FX];

    /* 1 = audio FX declared capabilities.requires_continuous_processing in
     * module.json; shim must never park the slot as fx_idle so stateful FX
     * (loopers, modulated delays) keep advancing internal time during silence. */
    int fx_requires_continuous[MAX_AUDIO_FX];
    
    /* Synth load error message */
    char synth_load_error[256];
} chain_instance_t;

/*
 * voice_bus[] must start at BUS_MIX_MAIN, not at calloc's 0, which is bus 1's
 * own index. A loop and not a memset: BUS_MIX_MAIN is -1, and a 0xFF byte-fill
 * only reads back as -1 by two's-complement luck.
 *
 * Called at create and on every synth load/unload, for the same reason
 * synth_split_voice_ids is cleared there — an assignment left over from the
 * previous module names a voice in a list that no longer exists.
 */
static inline void chain_reset_voice_bus(chain_instance_t *inst) {
    if (!inst) return;
    for (int i = 0; i < SPLIT_VOICES_MAX; i++) inst->voice_bus[i] = BUS_MIX_MAIN;
}

#define CHAIN_INTERNAL __attribute__((visibility("hidden")))

/*
 * Per-position metadata blocks, allocated and released together.
 *
 * `static inline` in the header rather than a function in chain_host.c because
 * chain_host.c is the one TU that cannot be compiled natively (it dlopens
 * plugins and owns the whole param surface), and several tests/host fixtures
 * build a chain_instance_t of their own with calloc. While these were inline
 * arrays a calloc'd instance was ready to use; now that they are pointers, a
 * fixture that skips this reads a null one. One definition, so production and
 * fixtures cannot disagree about what a usable instance is.
 *
 * ALL OR NOTHING: chain_alloc_position_storage frees what it managed to get and
 * answers 0, because a half-allocated instance is a null dereference in the
 * audio callback later, which is far worse than refusing to create the slot.
 */
/*
 * Did the plugin actually ANSWER the chain_params query?
 *
 * Every one of the three get_param routes (synth, audio FX, MIDI FX) asks the
 * sub-plugin for "chain_params" first and only renders its own module.json
 * fallback if the plugin declines. "Declined" was `result <= 0`, which is a
 * length test, and a length test cannot tell a real table from an empty one.
 *
 * impressive-chords v0.1.24 reads its chain_params from a JSON file at runtime
 * and returns the two characters "[]" when that file is not installed — which
 * it is not, in the shipped tarball. Length 2 counted as an answer, so the
 * host discarded the fifteen correct parameter declarations it had already
 * parsed out of that module's own module.json and served "[]" instead. The
 * Shadow UI's knob row then had no type for any of them and drove every one as
 * a float 0..1: an int wrote a fraction its atoi read as 0, an enum took
 * option 0. Reported as "i could see the values change, but when i release, it
 * reset to the default".
 *
 * An empty array is therefore treated as no answer, and the fallback runs. A
 * plugin that genuinely has no parameters loses nothing: the fallback finds no
 * parsed params either and the caller ends up returning -1, which the UI reads
 * exactly as it read "[]".
 *
 * Pure scan over a caller-owned buffer — no allocation, no I/O — because these
 * routes are serviced from the SPI callback.
 */
static inline int chain_params_answer_is_useful(const char *buf, int result) {
    if (result <= 0 || !buf) return 0;
    for (int i = 0; i < result && buf[i]; i++) {
        char c = buf[i];
        if (c == '[' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return 1;
    }
    return 0;
}

static inline void chain_free_position_storage(chain_instance_t *inst) {
    if (!inst) return;
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        free(inst->fx_params[i]);        inst->fx_params[i] = NULL;
        free(inst->fx_ui_hierarchy[i]);  inst->fx_ui_hierarchy[i] = NULL;
    }
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        free(inst->midi_fx_params[i]);       inst->midi_fx_params[i] = NULL;
        free(inst->midi_fx_ui_hierarchy[i]); inst->midi_fx_ui_hierarchy[i] = NULL;
    }
}

static inline int chain_alloc_position_storage(chain_instance_t *inst) {
    if (!inst) return 0;
    int ok = 1;
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        inst->fx_params[i] = (chain_param_info_t *)calloc(MAX_CHAIN_PARAMS,
                                                          sizeof(chain_param_info_t));
        inst->fx_ui_hierarchy[i] = (char *)calloc(1, CHAIN_UI_HIERARCHY_LEN);
        if (!inst->fx_params[i] || !inst->fx_ui_hierarchy[i]) ok = 0;
    }
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        inst->midi_fx_params[i] = (chain_param_info_t *)calloc(MAX_CHAIN_PARAMS,
                                                               sizeof(chain_param_info_t));
        inst->midi_fx_ui_hierarchy[i] = (char *)calloc(1, CHAIN_UI_HIERARCHY_LEN);
        if (!inst->midi_fx_params[i] || !inst->midi_fx_ui_hierarchy[i]) ok = 0;
    }
    if (!ok) chain_free_position_storage(inst);
    return ok;
}

/* Get current time in milliseconds (for knob acceleration) */
static inline uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Master preset registry — owned by chain_patch.c, read by v2_get_param. */
#define MAX_MASTER_PRESETS 64
CHAIN_INTERNAL extern char master_preset_names[MAX_MASTER_PRESETS][MAX_NAME_LEN];
CHAIN_INTERNAL extern char master_preset_paths[MAX_MASTER_PRESETS][MAX_PATH_LEN];
CHAIN_INTERNAL extern int master_preset_count;

/* ---- cross-TU internals (grouped by defining file) ---- */

/* chain_host.c */
CHAIN_INTERNAL void chain_log(const char *msg);
CHAIN_INTERNAL void parse_debug_log(const char *msg);
CHAIN_INTERNAL void v2_chain_log(chain_instance_t *inst, const char *msg);
CHAIN_INTERNAL int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name);
CHAIN_INTERNAL int v2_load_synth(chain_instance_t *inst, const char *module_name);
CHAIN_INTERNAL void v2_synth_panic(chain_instance_t *inst);
CHAIN_INTERNAL void v2_unload_all_audio_fx(chain_instance_t *inst);
CHAIN_INTERNAL void v2_unload_audio_fx_slot(chain_instance_t *inst, int slot);

/* chain_reorder.c — insert/remove/reorder a position by permuting the
 * instance's per-position arrays instead of reloading modules. 0-based. */
CHAIN_INTERNAL int chain_reorder_insert(chain_instance_t *inst, int is_midi, int at);
CHAIN_INTERNAL int chain_reorder_remove(chain_instance_t *inst, int is_midi, int at);
CHAIN_INTERNAL int chain_reorder_move(chain_instance_t *inst, int is_midi, int from, int to);
CHAIN_INTERNAL void v2_unload_synth(chain_instance_t *inst);

/* Bus allocation (chain_host.c). chain_bus_request_alloc is the RT-side half:
 * it marks the bus in use, flags it pending and starts the worker on first
 * use — a slot with no buses starts no thread. It allocates nothing itself.
 * Task 8's "bus<N>:create" dispatch is its caller. */
CHAIN_INTERNAL void chain_bus_request_alloc(chain_instance_t *inst, int bus);
/* The half of the above that does NOT claim the bus: flag it pending, start the
 * worker if this is the first use, post the semaphore. chain_bus.c uses it for
 * every change that is not a creation — including a DELETE, which must reach
 * the worker without setting in_use back to 1. RT-safe. */
CHAIN_INTERNAL void chain_bus_post_work(chain_instance_t *inst, int bus);
/* Stops and JOINS the worker; must be called before chain_bus_release_all. */
CHAIN_INTERNAL void chain_bus_worker_stop(chain_instance_t *inst);
/* Frees every bus's buffer, destroys its FX instances and dlcloses their
 * handles. Only safe once the worker is joined. */
CHAIN_INTERNAL void chain_bus_release_all(chain_instance_t *inst);

/* chain_bus.c — the "bus<N>:" parameter surface, the voice map and the
 * worker's reconcile step. Split out of chain_host.c for the same reason
 * chain_patch.c and chain_reorder.c were. */

/* RT side. Both return -1 for a key this file does not own, so the caller can
 * fall through to its existing ladders; chain_bus_set_param returns 0 when it
 * handled the key. */
CHAIN_INTERNAL int chain_bus_set_param(chain_instance_t *inst, int bus,
                                       const char *sub, const char *val);
CHAIN_INTERNAL int chain_bus_get_param(chain_instance_t *inst, int bus,
                                       const char *sub, char *buf, int buf_len);

/* Rebuild chain_instance_t::voice_bus from every bus's stored ids, refreshing
 * each bus's orphan_count. Call after any change to the assignments OR to the
 * synth's declared voice list — an id resolves against whatever module is
 * loaded NOW. RT-safe: a scan, no allocation. */
CHAIN_INTERNAL void chain_bus_rebuild_voice_map(chain_instance_t *inst);

/* Worker side: reconcile one bus's buffer and FX chain to the RT thread's
 * request. Runs on chain_bus_worker_fn (SCHED_OTHER) and is the ONLY place
 * bus FX are dlopen'd, instantiated and given their metadata.
 *
 * `stop` is polled between units of work — see chain_bus_worker_fn. It returns
 * as soon as it reads non-zero, leaving whatever it has already stored for
 * chain_bus_release_all to clean up. */
CHAIN_INTERNAL void chain_bus_worker_reconcile(chain_instance_t *inst, int bus,
                                               const int *stop);

/* Free a bus's per-position metadata. Called from chain_bus_release_all and
 * from the worker when a position empties. */
CHAIN_INTERNAL void chain_bus_free_fx_meta(slot_bus_t *bus, int pos);

/* Apply a parsed patch's bus section. RT side: shape and state are staged for
 * the worker, the voice map is rebuilt here. Answers the total number of
 * orphaned voice ids across every bus, which the caller reports. */
CHAIN_INTERNAL int chain_bus_apply_patch(chain_instance_t *inst, const patch_info_t *patch);

/* The slot-level bus keys ("buses:config", "buses:main_send<M>"), which name no
 * single bus and so cannot go through bus_route.h. Same return convention as
 * the indexed pair above. */
CHAIN_INTERNAL int chain_bus_slot_set_param(chain_instance_t *inst, const char *sub, const char *val);
CHAIN_INTERNAL int chain_bus_slot_get_param(chain_instance_t *inst, const char *sub, char *buf, int buf_len);

/* Drop every bus back to its resting state: no voices, no sends, no FX, buffer
 * retired. Used by "clear" and before a patch load, for the same reason the
 * LFOs and knob mappings are cleared there — bus config is per-SLOT state and
 * would otherwise outlive the set that defined it. */
CHAIN_INTERNAL void chain_bus_clear_all(chain_instance_t *inst);

/* chain_json.c */
CHAIN_INTERNAL const char *bounded_strstr(const char *start, const char *end, const char *needle);
CHAIN_INTERNAL int json_get_float(const char *json, const char *key, float *out);
CHAIN_INTERNAL int json_get_int(const char *json, const char *key, int *out);
CHAIN_INTERNAL int json_get_bool(const char *json, const char *key, int *out);
CHAIN_INTERNAL int json_get_int_in_section(const char *json, const char *section_key, const char *key, int *out);
CHAIN_INTERNAL int json_get_bool_in_section(const char *json, const char *section_key, const char *key, int *out);
CHAIN_INTERNAL int json_get_section_bounds(const char *json, const char *section_key, const char **out_start, const char **out_end);
CHAIN_INTERNAL int json_get_string(const char *json, const char *key, char *out, int out_len);
CHAIN_INTERNAL int json_get_string_in_section(const char *json, const char *section_key, const char *key, char *out, int out_len);
CHAIN_INTERNAL int json_decode_quoted_string(const char *quoted, const char *limit,
                                             char *out, int out_len);

/* chain_params.c */
CHAIN_INTERNAL float dsp_value_to_float(const char *val_str, chain_param_info_t *pinfo, float fallback);
CHAIN_INTERNAL chain_param_info_t* find_param_by_key(chain_instance_t *inst, const char *target, const char *key);
CHAIN_INTERNAL chain_param_info_t *find_param_info(chain_param_info_t *params, int count, const char *key);
CHAIN_INTERNAL int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len);
CHAIN_INTERNAL int is_smoothable_float(const char *val, float *out_value);
CHAIN_INTERNAL chain_param_info_t *knob_find_param(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL void knob_forward_value(chain_instance_t *inst, const char *target, const char *param, const char *val_str);

/* Echo one chain knob's current value to the external port as CC 102-109 on
 * the slot's receive channel. No-op unless the patch opts in via knob_cc_out.
 * Audio-thread safe (ring enqueue only). `idx` indexes inst->knob_mappings. */
CHAIN_INTERNAL void knob_emit_cc_out(chain_instance_t *inst, int idx);

/* Echo every mapped chain knob. Used after a patch load or knob remap, where
 * values change without any per-knob event for the controller to have seen. */
CHAIN_INTERNAL void knob_emit_cc_out_all(chain_instance_t *inst);
/* Render a parsed chain_param_info_t table as the chain_params JSON array the
 * shadow UI reads. Extracted for chain_bus.c, which would otherwise be a FOURTH
 * hand-written copy of this loop; the three existing copies in chain_host.c's
 * synth / fx / midi_fx get_param routes are deliberately left alone here, as
 * folding them in is a change to three live read paths and not this task's. */
CHAIN_INTERNAL int chain_params_emit_json(const chain_param_info_t *params, int count,
                                          char *buf, int buf_len);
CHAIN_INTERNAL int parse_chain_params(const char *module_path, chain_param_info_t *params, int *count);
CHAIN_INTERNAL int parse_chain_params_array_json(const char *json_array, chain_param_info_t *params, int max_params);
CHAIN_INTERNAL int parse_ui_hierarchy_cache(const char *module_path, char *out, int out_len);
CHAIN_INTERNAL void smoother_reset(param_smoother_t *smoother);
CHAIN_INTERNAL void smoother_set_target(param_smoother_t *smoother, const char *key, float value);
CHAIN_INTERNAL int smoother_update(param_smoother_t *smoother);

/* chain_mod.c */
CHAIN_INTERNAL void chain_mod_apply_effective_value(chain_instance_t *inst, mod_target_state_t *entry, int force_write);
CHAIN_INTERNAL void chain_mod_clear_source(void *ctx, const char *source_id);
CHAIN_INTERNAL void chain_mod_clear_target_entries(chain_instance_t *inst, const char *target, int restore_base);
CHAIN_INTERNAL int chain_mod_emit_value(void *ctx, const char *source_id, const char *target, const char *param, float signal, float depth, float offset, int bipolar, int enabled);
CHAIN_INTERNAL mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL int chain_mod_get_base_for_plain_key(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_get_base_for_subkey(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_get_effective_for_subkey(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_get_modulated_for_subkey(chain_instance_t *inst, const char *target, const char *subkey, char *buf, int buf_len);
CHAIN_INTERNAL int chain_mod_is_target_active(chain_instance_t *inst, const char *target, const char *param);
CHAIN_INTERNAL int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *target);
CHAIN_INTERNAL void chain_mod_update_base_from_set_param(chain_instance_t *inst, const char *target, const char *param, const char *val);

/* chain_midi.c */
CHAIN_INTERNAL int chain_get_clock_status(void);
CHAIN_INTERNAL int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name);
CHAIN_INTERNAL int v2_load_midi_fx_slot(chain_instance_t *inst, int slot, const char *fx_name);
CHAIN_INTERNAL void v2_on_midi(void *instance, const uint8_t *msg, int len, int source);
CHAIN_INTERNAL void v2_tick_midi_fx(chain_instance_t *inst, int frames);
CHAIN_INTERNAL void v2_unload_all_midi_fx(chain_instance_t *inst);
CHAIN_INTERNAL void v2_unload_midi_fx_slot(chain_instance_t *inst, int slot);

/* chain_patch.c */
CHAIN_INTERNAL int delete_master_preset(int index);
CHAIN_INTERNAL int load_master_preset_json(int index, char *buf, int buf_len);
CHAIN_INTERNAL int save_master_preset(const char *json_str);
CHAIN_INTERNAL void scan_master_presets(void);
CHAIN_INTERNAL int update_master_preset(int index, const char *json_str);
CHAIN_INTERNAL int v2_delete_patch(chain_instance_t *inst, int index);
CHAIN_INTERNAL int v2_load_from_patch_info(chain_instance_t *inst, patch_info_t *patch);
CHAIN_INTERNAL int v2_load_patch(chain_instance_t *inst, int patch_idx);
CHAIN_INTERNAL int v2_parse_patch_file(chain_instance_t *inst, const char *path, patch_info_t *patch);
CHAIN_INTERNAL int v2_save_patch(chain_instance_t *inst, const char *json_data);
CHAIN_INTERNAL int v2_scan_patches(chain_instance_t *inst);
CHAIN_INTERNAL int v2_update_patch(chain_instance_t *inst, int index, const char *json_data);


#endif /* CHAIN_INTERNAL_H */
