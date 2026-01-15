/*
 * Audio FX Plugin API v2
 *
 * Instance-based interface for audio effect plugins that process stereo audio.
 * v2 allows multiple instances of the same effect plugin to coexist with
 * independent state.
 */

#ifndef AUDIO_FX_API_V2_H
#define AUDIO_FX_API_V2_H

#include <stdint.h>
#include "plugin_api_v1.h"  /* For host_api_v1_t */

#define AUDIO_FX_API_VERSION_2 2
#define AUDIO_FX_INIT_V2_SYMBOL "move_audio_fx_init_v2"

/* Audio FX plugin interface v2 - instance-based */
typedef struct audio_fx_api_v2 {
    uint32_t api_version;

    /* Create instance - returns opaque instance pointer, or NULL on failure
     * module_dir: path to module directory
     * config_json: JSON string from configuration, or NULL
     */
    void* (*create_instance)(const char *module_dir, const char *config_json);

    /* Destroy instance - clean up and free instance */
    void (*destroy_instance)(void *instance);

    /* Process audio in-place (stereo interleaved int16) */
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);

    /* Set a parameter by key/value */
    void (*set_param)(void *instance, const char *key, const char *val);

    /* Get a parameter value, returns bytes written or -1 */
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);

} audio_fx_api_v2_t;

/* Entry point function type */
typedef audio_fx_api_v2_t* (*audio_fx_init_v2_fn)(const host_api_v1_t *host);

#endif /* AUDIO_FX_API_V2_H */
