/*
 * MIDI FX Plugin API v1
 *
 * API for MIDI effects that transform, generate, or filter MIDI messages.
 * Examples: chord generators, arpeggiators, note filters, velocity curves.
 *
 * Unlike Audio FX which process audio buffers, MIDI FX:
 * - Transform incoming MIDI events (may output 0, 1, or multiple messages)
 * - May generate MIDI events on a timer (arpeggiator)
 * - Maintain state between calls (held notes, sequence position)
 *
 * A MIDI FX module exports one symbol:
 *   midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host);
 *
 * Called once at load time. Returns a pointer to a static vtable.
 * Store the host pointer as a module-level static for use in tick():
 *
 *   static const host_api_v1_t *g_host = NULL;
 *   midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
 *       g_host = host;
 *       return &g_api;
 *   }
 *
 * Maximum 2 native MIDI FX per chain (MAX_MIDI_FX in chain_host.c).
 */

#ifndef MIDI_FX_API_V1_H
#define MIDI_FX_API_V1_H

#include <stdint.h>

#define MIDI_FX_API_VERSION 1
#define MIDI_FX_MAX_OUT_MSGS 16  /* Max messages that can be output per call */
#define MIDI_FX_INIT_SYMBOL "move_midi_fx_init"

/* Forward declaration */
struct host_api_v1;

/*
 * MIDI FX Plugin API
 */
typedef struct midi_fx_api_v1 {
    uint32_t api_version;  /* Must be MIDI_FX_API_VERSION */

    /*
     * Create a new instance of this MIDI FX.
     * Called when loading the FX into a chain slot.
     *
     * - Allocate with calloc(1, sizeof(YourInstance))
     * - Set ALL parameter defaults here — do not rely on set_param being called
     * - Return NULL on allocation failure
     *
     * @param module_dir  Path to the module directory (for loading resources)
     * @param config_json Optional JSON configuration string, or NULL
     * @return Opaque instance pointer, or NULL on failure
     */
    void* (*create_instance)(const char *module_dir, const char *config_json);

    /*
     * Destroy an instance.
     * Called when unloading the FX from a chain slot.
     *
     * @param instance  Instance pointer from create_instance
     */
    void (*destroy_instance)(void *instance);

    /*
     * Process an incoming MIDI message.
     * May output 0, 1, or multiple messages in response.
     *
     * For simple transformations (transpose, velocity curve):
     *   - Return 1 message with the transformed data
     *
     * For chord generators:
     *   - Return multiple messages (root + chord notes)
     *
     * For filters:
     *   - Return 0 to block the message, 1 to pass through
     *
     * For arpeggiators receiving note-on:
     *   - Return 0 (arp will generate notes via tick())
     *   - Store the note internally
     *
     * Rules:
     * - Pass through unrecognized messages by copying to out_msgs
     * - Handle velocity-0 note-on as note-off
     * - Track active notes so they can be cleaned up (see note lifecycle below)
     * - Always check count < max_out before writing each output message
     * - MIDI transport bytes (0xFA/0xFB/0xFC) are NOT forwarded to plugins
     *   by the chain host — use get_clock_status() in tick() for transport sync
     *
     * @param instance    Instance pointer
     * @param in_msg      Incoming MIDI message (1-3 bytes)
     * @param in_len      Length of incoming message
     * @param out_msgs    Output buffer for messages (each up to 3 bytes)
     * @param out_lens    Output buffer for message lengths
     * @param max_out     Maximum number of output messages (buffer size)
     * @return Number of output messages written (0 to max_out)
     */
    int (*process_midi)(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

    /*
     * Tick function called each audio render block.
     * Used for time-based effects like arpeggiators.
     *
     * - Use frames to advance timing counters
     * - For transport-synced modules: call host->get_clock_status() here
     * - For free-running modules: do NOT call get_clock_status() or get_bpm()
     *   (causes SIGSEGV on some firmware if MIDI Clock Out is not enabled)
     * - Do not allocate memory or block in tick
     * - Emit note-offs before note-ons in the same frame
     *
     * @param instance    Instance pointer
     * @param frames      Number of audio frames in this block (typically 128)
     * @param sample_rate Audio sample rate (typically 44100)
     * @param out_msgs    Output buffer for generated MIDI messages
     * @param out_lens    Output buffer for message lengths
     * @param max_out     Maximum number of output messages
     * @return Number of output messages generated (0 to max_out)
     */
    int (*tick)(void *instance,
                int frames, int sample_rate,
                uint8_t out_msgs[][3], int out_lens[],
                int max_out);

    /*
     * Set a parameter value.
     *
     * val is always a string. Move may send params in different formats
     * depending on context:
     *   - Raw ints: "7", "-12"
     *   - Raw floats: "7.0000", "64.0000"
     *   - Normalized floats: "0.5000"
     * Parse with atof(), atoi(), or strcmp() as appropriate.
     * Validate and clamp before applying.
     *
     * The "state" key receives a JSON string for state recall (e.g.,
     * {"min":1,"max":127}). Parse and apply all params from it.
     *
     * @param instance  Instance pointer
     * @param key       Parameter name (e.g., "mode", "bpm", "type")
     * @param val       Parameter value as string
     */
    void (*set_param)(void *instance, const char *key, const char *val);

    /*
     * Get a parameter value.
     *
     * IMPORTANT: Must return snprintf(buf, buf_len, ...) for known params.
     * Returning 0 silently breaks param display, chain editing, and state
     * recall. Return -1 for unknown keys.
     *
     * Special keys the chain host queries:
     *   "state"        — return JSON with all params for state persistence
     *   "chain_params" — return JSON array of parameter metadata for the
     *                     Shadow UI parameter editor (type, min, max, step)
     *   "ui_hierarchy" — return JSON hierarchy for Shadow UI menu structure
     *
     * Example:
     *   if (strcmp(key, "min") == 0)
     *       return snprintf(buf, buf_len, "%d", inst->vel_min);
     *   return -1;
     *
     * @param instance  Instance pointer
     * @param key       Parameter name
     * @param buf       Output buffer for value string
     * @param buf_len   Size of output buffer
     * @return Length of value written, or -1 if key not found
     */
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);

} midi_fx_api_v1_t;

/*
 * Init function signature.
 * Each MIDI FX module must export this function.
 *
 * @param host  Host API for callbacks (logging, clock, MIDI send, etc.)
 * @return Pointer to the plugin's API struct (must remain valid until destroy)
 */
typedef midi_fx_api_v1_t* (*midi_fx_init_fn)(const struct host_api_v1 *host);

#endif /* MIDI_FX_API_V1_H */
