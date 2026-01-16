/*
 * Audio FX Plugin API v1
 *
 * Interface for audio effect plugins that process stereo audio in-place.
 */

#ifndef AUDIO_FX_API_V1_H
#define AUDIO_FX_API_V1_H

#include <stdint.h>
#include "plugin_api_v1.h"  /* For host_api_v1_t */

#define AUDIO_FX_API_VERSION 1
#define AUDIO_FX_INIT_SYMBOL "move_audio_fx_init_v1"

/* Audio FX plugin interface */
typedef struct audio_fx_api_v1 {
    uint32_t api_version;

    /* Called when effect is loaded */
    int (*on_load)(const char *module_dir, const char *config_json);

    /* Called when effect is unloaded */
    void (*on_unload)(void);

    /* Process audio in-place (stereo interleaved int16) */
    void (*process_block)(int16_t *audio_inout, int frames);

    /* Set a parameter by key/value */
    void (*set_param)(const char *key, const char *val);

    /* Get a parameter value, returns bytes written or -1 */
    int (*get_param)(const char *key, char *buf, int buf_len);

} audio_fx_api_v1_t;

/* Entry point function type */
typedef audio_fx_api_v1_t* (*audio_fx_init_v1_fn)(const host_api_v1_t *host);

#endif /* AUDIO_FX_API_V1_H */
