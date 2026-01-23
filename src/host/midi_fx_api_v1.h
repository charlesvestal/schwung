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
     * @param instance  Instance pointer
     * @param key       Parameter name (e.g., "mode", "bpm", "type")
     * @param val       Parameter value as string
     */
    void (*set_param)(void *instance, const char *key, const char *val);

    /*
     * Get a parameter value.
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
 * @param host  Host API for callbacks (logging, etc.)
 * @return Pointer to the plugin's API struct
 */
typedef midi_fx_api_v1_t* (*midi_fx_init_fn)(const struct host_api_v1 *host);

#endif /* MIDI_FX_API_V1_H */
