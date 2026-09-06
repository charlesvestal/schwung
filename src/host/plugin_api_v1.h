/*
 * Schwung Plugin API v1
 *
 * Stable ABI for DSP modules loaded by the host runtime.
 * Modules are .so files loaded via dlopen() and must export move_plugin_init_v1().
 *
 * ===========================================================================
 * THREADING CONTRACT — READ THIS FIRST
 * ===========================================================================
 *
 * THERE IS NO CONTROL THREAD. Every entry point below runs on the SPI audio
 * callback: SCHED_FIFO 70, pinned to core 3, with roughly 2370 microseconds
 * of slack per 128-frame block.
 *
 * Both numbers were measured and both replace older ones that are still
 * quoted in places. FIFO 70: the 2026-08-22 RT-thread audit found nothing
 * anywhere in the MoveOriginal process above 70 (the SPI *driver* is a
 * separate kernel thread and is a different question). 2370 us: the
 * 2026-08-26 SPI frame tally found the transfer itself is 389 us, not the
 * ~2 ms long assumed -- the rest of the ioctl is idle IRQ wait, which is
 * slack you may spend. Arm them yourself rather than trusting this comment:
 * `rt_thread_audit_on` and `spi_tally_on`.
 *
 *      create_instance     <- yes, this one too
 *      destroy_instance
 *      set_param           <- yes, this one too
 *      get_param           <- yes, this one too
 *      on_midi / process_midi
 *      render_block / process_block / tick
 *
 * This is the single most misunderstood thing about writing a Schwung module.
 * A 2026-08 audit of all 113 catalogued modules found ~150 confirmed realtime
 * violations, and several carried comments asserting the opposite in so many
 * words — "control-thread only (blocking dir + file I/O)", "run on the control
 * thread, NEVER from process_block, so this malloc is realtime-safe", "safe
 * because it runs in the MIDI callback (not the RT render thread)". Every one
 * of those premises is false, and each produced a multi-megabyte blocking
 * operation on the audio thread. If you take one thing from this header, take
 * this paragraph.
 *
 * FORBIDDEN in all of the calls listed above:
 *
 *   - file I/O: fopen/fread/fwrite/open/read/stat/opendir/readdir/mkdir/unlink
 *   - allocation or free: malloc/calloc/realloc/free/new/delete
 *   - locks held by any non-RT thread (and any lock without PRIO_INHERIT)
 *   - fork/exec/system/popen, dlopen
 *   - logging of any kind, including host->log and a bare fprintf(stderr, ...)
 *     -- stderr is unbuffered, so that is a write() syscall even when your
 *     debug flag is off
 *   - unbounded work: an FFT, a full-buffer memset, a directory sort
 *
 * The symptom is not a glitch in your module. It is a device-wide audio
 * dropout, because you are holding the thread that services every other
 * module's audio and Move's own.
 *
 * THREADS INHERIT SCHED_FIFO 70. pthread_create() called from any of the
 * above hands your worker the callback's realtime priority. Move's own
 * `Link Main` thread runs at SCHED_FIFO 35, so an inherited-priority worker
 * starves Move's audio publisher and produces exactly the dropouts you were
 * trying to avoid by going off-thread. Every worker must demote itself as its
 * FIRST action:
 *
 *      struct sched_param sp = { .sched_priority = 0 };
 *      sched_setscheduler(0, SCHED_OTHER, &sp);
 *      // and keep core 3 free for SPI:
 *      cpu_set_t set; CPU_ZERO(&set);
 *      CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
 *      sched_setaffinity(0, sizeof(set), &set);
 *
 * Reference implementations that get this right: schwung-keydetect
 * (keyfinder_wrapper.cpp -- demotes and pins) and schwung-airwindows
 * (clap_fx.cpp loader_thread_fn -- demotes, with a comment naming the
 * inheritance problem). The audit found at least 14 modules that do not.
 *
 * WHAT TO DO INSTEAD. Load files, allocate buffers and build lists on your own
 * SCHED_OTHER worker thread, and have the audio path pick up the result by
 * publishing a pointer. Keep get_param cheap: a `get_param` that rescans a
 * directory is served on this thread once per repaint, not once per click.
 * If you must do work at create time, prefer doing it lazily on the worker and
 * rendering silence until it lands.
 *
 * ONE QUALIFICATION, and it does not weaken any rule above. An audio FX loaded
 * into a chain SLOT is created, configured and processed on the callback, as
 * described. An audio FX loaded into a chain BUS insert position is loaded by
 * the chain's bus worker (SCHED_OTHER, cores 0-2): its dlopen, create_instance,
 * destroy_instance and the set_param that restores its saved state run THERE,
 * while process_block, on_midi and every live set_param/get_param still run on
 * the callback. So:
 *
 *   - You still may not do any of the forbidden things above at create time.
 *     You have no way to know which of the two you were loaded as, and the slot
 *     case — the common one — is the callback.
 *   - Process-global initialisation must be thread-safe. The same module can be
 *     constructed on the worker for a bus and on the callback for a slot AT THE
 *     SAME TIME. Per-instance state is unaffected; a shared static table, a
 *     lazily-built wavetable or a library init that is not reentrant is not.
 *
 * "There is no control thread" remains the rule to write code against. This is
 * the one place the host does not hold still, and it buys you nothing.
 *
 * See docs/REALTIME_SAFETY.md for the measurements behind all of this.
 * ===========================================================================
 */

#ifndef MOVE_PLUGIN_API_V1_H
#define MOVE_PLUGIN_API_V1_H

#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1

/* Audio constants */
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_AUDIO_OUT_OFFSET 256
#define MOVE_AUDIO_IN_OFFSET (2048 + 256)
#define MOVE_AUDIO_BYTES_PER_BLOCK 512

/* MIDI source identifiers */
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2
#define MOVE_MIDI_SOURCE_HOST 3  /* Host-generated (clock, etc) */
#define MOVE_MIDI_SOURCE_FX_BROADCAST 4  /* Broadcast to audio FX only (skip synth) */

/* Clock status identifiers for host_api_v1.get_clock_status() */
#define MOVE_CLOCK_STATUS_UNAVAILABLE 0  /* Clock output not available/configured */
#define MOVE_CLOCK_STATUS_STOPPED 1      /* Clock available, transport stopped */
#define MOVE_CLOCK_STATUS_RUNNING 2      /* Clock available, transport running */

/* Optional modulation callbacks for chain-owned runtime modulation buses.
 * Sub-plugins can publish temporary modulation contributions without writing
 * target base values directly.
 */
typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal,
                                      float depth,
                                      float offset,
                                      int bipolar,
                                      int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

/*
 * Host API - provided by host to plugin during initialization
 */
typedef struct host_api_v1 {
    uint32_t api_version;

    /* Audio constants */
    int sample_rate;
    int frames_per_block;

    /* Direct mailbox access (use with care) */
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;

    /* Logging */
    void (*log)(const char *msg);

    /* MIDI send functions
     * msg: 4-byte USB-MIDI packet [cable|CIN, status, data1, data2]
     * len: number of bytes (typically 4)
     * Returns: bytes queued, or 0 on failure
     */
    int (*midi_send_internal)(const uint8_t *msg, int len);

    /* midi_send_external queues onto the shim's lock-free ROUTE_EXTERNAL ring,
     * drained into Move's 80-byte MIDI_OUT region once per audio block. Safe
     * from every entry point (they are all the SPI callback — see the contract
     * at the top of this file): no allocation, no syscall, no logging.
     *
     * IT IS NOT A DELIVERY GUARANTEE, and the 0 return is the whole contract.
     * The ring drops-newest when full, and those 80 bytes are shared with
     * Move's own output, the LED queue and the shadow UI. Retry on 0; never
     * record a packet as delivered on a 0. A caller that mistakes "queued" for
     * "sent" leaves whatever it was reporting permanently stale.
     *
     * CHAIN SUB-PLUGINS GET THIS TOO, as of chain knob CC out — the chain host
     * copies its own host_api down to every synth, audio FX and MIDI FX it
     * loads. It was NULL there for years, so a module that guards on NULL has
     * silently been a no-op inside a chain slot and will now start emitting to
     * USB-A. If that is not what your module wants in a chain, check
     * slot_recv_channel() first: it answers -2 when the instance is not
     * slot-registered. */
    int (*midi_send_external)(const uint8_t *msg, int len);

    /* Clock status query for sync-aware plugins.
     * Returns one of MOVE_CLOCK_STATUS_*.
     */
    int (*get_clock_status)(void);

    /* Optional runtime modulation callbacks (NULL if unsupported). */
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;

    /* Tempo query — returns current BPM (120.0 default).
     * Uses sampler_get_bpm() fallback chain: MIDI clock → set tempo → settings → 120.
     * NULL if host does not support tempo. */
    float (*get_bpm)(void);

    /* Inject a USB-MIDI packet into Move's MIDI_IN, as if it had arrived at
     * the hardware.
     *
     * THE CABLE NIBBLE IS PRESERVED, AND IT CHOOSES THE ROUTE. The drain
     * memcpy's the packet into a MIDI_IN slot verbatim
     * (shadow_overtake_midi.c), so the caller — not the host — decides how
     * Move reads it:
     *
     *   cable 0  Move treats the event as its own surface (pad/button).
     *            Use this to simulate a press.
     *   cable 2  Move routes it by CHANNEL to the track instrument, exactly
     *            as it would an external USB-A device — AND ECHOES IT BACK
     *            OUT MIDI_OUT CABLE 2, i.e. to whatever is plugged into
     *            USB-A. This is how a chain MIDI FX in Pre mode reaches an
     *            external synth; it is also a loop back into any chain slot
     *            listening on that channel, so a note-generating caller must
     *            filter its own echo (see pre_mode_is_echo in chain_midi.c).
     *
     * The channel byte must be the slot's RECV channel, not forward_channel
     * — see slot_recv_channel below.
     *
     * msg: 4-byte USB-MIDI packet [cable|CIN, status, data1, data2]
     * len: must be 4
     * Returns: bytes queued, or 0 on failure (SHM unavailable, ring full).
     *
     * NULL if host does not support MIDI-IN injection (non-shadow host).
     *
     * DELIVERY IS NOT PER-TICK PACED, it is gated on MIDI_IN being idle: the
     * drain runs only after two consecutive frames with every MIDI_IN slot
     * empty, then fills consecutive slots and stops at the first occupied one
     * (shadow_midi.c shadow_drain_midi_inject). Under sustained hardware input
     * it may not run at all, so treat a queued packet as queued — never as
     * delivered — and keep bursts small. */
    int (*midi_inject_to_move)(const uint8_t *msg, int len);

    /* Return the receive channel for the slot owning this plugin instance.
     * -1 = All (no filter), 0-15 = specific channel byte, -2 = instance not
     * registered (e.g. master FX, host-level plugin).
     *
     * Use this to address Move tracks via midi_inject_to_move: the inject
     * channel must be the slot's recv channel, NOT the slot's
     * forward_channel (which is purely an internal synth-side routing hint,
     * e.g. minijv part 6). NULL if the host doesn't expose slot context. */
    int (*slot_recv_channel)(void *instance);

    /* Beats since transport start of the active clock source (Move's native
     * sequencer, or an internal module's emitted clock), derived from
     * 24-PPQN realtime ticks and interpolated per block. Returns < 0 when
     * no transport is running — callers must fall back (e.g. LFO free-run).
     * Appended in 2026-07; may be NULL on older hosts, always guard. */
    double (*get_beat_position)(void);

    /* Reserved tail — a RUN OF NULLS, and it is load-bearing.
     *
     * Every field above is guarded by callers as `if (host->fn) host->fn()`,
     * which is only sound while a read inside the struct is the only read that
     * can happen. It isn't: a module's copy of this header can declare a field
     * we do not have, and the guard then tests memory belonging to somebody
     * else.
     *
     * That is not hypothetical. breakbeat's copy appends
     * `float (*get_project_bpm)(void)` after get_beat_position -- a callback
     * NO Schwung has ever provided -- so it resolves to +120, one past our
     * last field. Its own comment reads "Appended host callbacks. Keep these
     * at the end for ABI compatibility", which is the right instinct applied
     * in the wrong direction: appending lets a module be OLDER than the host,
     * never newer. A module cannot extend this struct from its side.
     *
     * The same binary calls the real get_bpm() at +88, so the two offsets sit
     * side by side in one disassembly. The struct a chain sub-plugin gets is
     * chain_instance_t::subplugin_host_api, so +120 read the NEXT MEMBER of
     * that instance — non-NULL, so the module's own guard passed, and the blr
     * jumped into the heap. SIGSEGV on the SPI callback at load, which takes
     * MoveOriginal down with it and boot-loops the device if the slot is
     * restored.
     *
     * Zeroed by construction: every instance of this struct is a static or a
     * member of a calloc'd instance, and chain_host memcpy's sizeof(). So an
     * over-read by up to 64 bytes finds NULL and the caller's existing guard
     * does what it was always written to do.
     *
     * This does NOT make the ABI extensible. Appending a real field still
     * requires modules to be rebuilt; the reserved run only buys a safe
     * failure instead of a crash. Shrink it and old binaries start reaching
     * past it again — so consume from the FRONT when adding a field, and
     * never reduce the total. */
    void *reserved[8];

} host_api_v1_t;

/*
 * Plugin API - implemented by plugin, returned to host
 */
typedef struct plugin_api_v1 {
    uint32_t api_version;

    /* Lifecycle */

    /* Called after dlopen, before any other calls
     * module_dir: path to module directory (e.g., "/data/.../modules/sf2")
     * json_defaults: JSON string from module.json "defaults" section, or NULL
     * Returns: 0 on success, non-zero on failure
     */
    int (*on_load)(const char *module_dir, const char *json_defaults);

    /* Called before dlclose */
    void (*on_unload)(void);

    /* Events */

    /* Called for each MIDI message
     * msg: 3 bytes [status, data1, data2]
     * len: number of bytes (typically 3)
     * source: MOVE_MIDI_SOURCE_INTERNAL or MOVE_MIDI_SOURCE_EXTERNAL
     */
    void (*on_midi)(const uint8_t *msg, int len, int source);

    /* Set a parameter by name (stringly-typed for v1 simplicity)
     * key: parameter name (e.g., "preset", "soundfont_path")
     * val: parameter value as string
     */
    void (*set_param)(const char *key, const char *val);

    /* Get a parameter by name
     * key: parameter name
     * buf: output buffer
     * buf_len: size of output buffer
     * Returns: length written, or -1 if not found
     */
    int (*get_param)(const char *key, char *buf, int buf_len);

    /* Get error message if module is in error state
     * buf: output buffer
     * buf_len: size of output buffer
     * Returns: length written, or 0 if no error
     */
    int (*get_error)(char *buf, int buf_len);

    /* Audio rendering */

    /* Render one block of audio
     * out_interleaved_lr: output buffer for stereo interleaved int16 samples
     *                     layout: [L0, R0, L1, R1, ..., L127, R127]
     * frames: number of frames to render (always MOVE_FRAMES_PER_BLOCK)
     */
    void (*render_block)(int16_t *out_interleaved_lr, int frames);

} plugin_api_v1_t;

/*
 * Plugin entry point - must be exported by all plugins
 *
 * host: pointer to host API struct (valid for plugin lifetime)
 * Returns: pointer to plugin API struct (must remain valid until on_unload)
 */
typedef plugin_api_v1_t* (*move_plugin_init_v1_fn)(const host_api_v1_t *host);

#define MOVE_PLUGIN_INIT_SYMBOL "move_plugin_init_v1"

/*
 * Plugin API v2 - Instance-based API for multi-instance support
 *
 * v2 plugins return an instance pointer from create_instance() and all
 * subsequent calls pass that instance pointer. This allows multiple
 * instances of the same plugin to coexist with independent state.
 *
 * Plugins can export BOTH v1 and v2 symbols during migration.
 * Hosts should prefer v2 when available.
 */

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;

    /* Create instance - returns opaque instance pointer, or NULL on failure
     * module_dir: path to module directory
     * json_defaults: JSON string from module.json "defaults" section, or NULL
     */
    void* (*create_instance)(const char *module_dir, const char *json_defaults);

    /* Destroy instance - clean up and free instance */
    void (*destroy_instance)(void *instance);

    /* All callbacks take instance as first parameter */
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);

} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);

#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"

/*
 * ===========================================================================
 * OPTIONAL: PER-VOICE RENDER (move_plugin_render_split)
 * ===========================================================================
 *
 * A sound generator can offer to render named voices into SEPARATE buffers, so
 * the Signal Chain can put a kick and a snare on different insert chains and
 * different sends. Two things opt in, and both are optional -- a module that
 * does neither is rendered exactly as it always was.
 *
 *   1. get_param("split_voices") answers a FLAT ORDERED JSON array:
 *
 *          [{"id":"kick","label":"Kick"},{"id":"snare","label":"Snare"}]
 *
 *      ENTRY i IS BUFFER i. The host resolves the bus->voice map in C on the
 *      SPI callback and its JSON helpers cannot walk ui_hierarchy's `levels`
 *      in order, so this list is flat and its ORDER is the contract. Never
 *      reorder it between versions: a bus stores voice IDS, and an id that no
 *      longer resolves is reported as an orphan rather than silently
 *      re-pointed. Adding a voice at the END is safe; inserting one is not.
 *      Answer "" (or do not serve the key) to say "I cannot split".
 *
 *   2. Export this symbol -- NOT a field on plugin_api_v2_t:
 *
 *          void move_plugin_render_split(void *instance,
 *                                        int16_t *const *voice_out,
 *                                        int n_voices,
 *                                        int16_t *main_out, int frames);
 *
 *      A SEPARATE EXPORTED SYMBOL ON PURPOSE. Appending to plugin_api_v2_t is
 *      what boot-looped a device via breakbeat's header drift: a module cannot
 *      extend the ABI from its side, and a guarded read of a field the host
 *      does not have tests memory belonging to somebody else. A dlsym'd symbol
 *      is absent-or-present, with no offset to get wrong.
 *
 * IT ACCUMULATES -- the opposite of render_block, which overwrites. The host
 * clears every destination before the call, main_out included. NEVER memset a
 * destination yourself: main_out and the voice_out[] entries alias each other,
 * so clearing one of them is clearing somebody else's audio for that frame.
 *
 * main_out IS FOR AUDIO THAT BELONGS TO NO VOICE -- a drum bus, a mix
 * compressor, a global filter, an internal reverb return. It is the same
 * buffer an unassigned voice is handed, so it is usually reachable through
 * voice_out[] as well; it is passed explicitly so that it is reachable even
 * when EVERY voice is on a bus and no voice_out[] entry points at main. A
 * module with no master section simply ignores it. Accumulate into it under
 * exactly the same rules as voice_out[]: never more than `frames` frames, and
 * never an overwrite.
 *
 * ITS voice_out[] ENTRIES ALIAS. Two voices routed to the same bus are handed
 * the SAME pointer, so their sum happens inside your own render loop with no
 * mixing pass at all, and a voice on no bus is handed the main output buffer,
 * so the sparse case costs nothing. Therefore:
 *
 *   - ACCUMULATE (out[i] += sample, saturating). Overwriting turns two voices
 *     on one bus into whichever one wrote last.
 *   - NEVER write more than `frames` frames (frames * 2 samples) into any
 *     voice_out[] entry. Those buffers are shared, so an overrun is a
 *     different bus's audio, not your own tail. Nothing checks this for you.
 *   - Both entry points must be STATE-COMPATIBLE. The host switches between
 *     render_split and render_block AT RUNTIME, PER FRAME, on whether any of
 *     your voices is currently assigned to a bus -- assigning one voice on the
 *     shadow UI flips your active entry point mid-stream with no reload. Same
 *     voice allocator, same envelope/LFO/phase state, or the flip is audible.
 *
 * n_voices is what the host parsed from your own list (clamped to its own
 * maximum), so it can be SHORTER than the list you published. Index
 * voice_out[] only in [0, n_voices).
 *
 * Runs on the SPI callback, under every rule at the top of this header.
 * See docs/CHAIN.md ("Buses") and src/host/bus_mix.h.
 * ===========================================================================
 */

#endif /* MOVE_PLUGIN_API_V1_H */
