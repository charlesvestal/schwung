/*
 * Move Anything Plugin API v1
 *
 * Stable ABI for DSP modules loaded by the host runtime.
 * Modules are .so files loaded via dlopen() and must export move_plugin_init_v1().
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
    int (*midi_send_external)(const uint8_t *msg, int len);

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

#endif /* MOVE_PLUGIN_API_V1_H */
