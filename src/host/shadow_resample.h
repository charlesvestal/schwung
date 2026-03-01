/* shadow_resample.h - Native resample bridge
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_RESAMPLE_H
#define SHADOW_RESAMPLE_H

#include <stdint.h>
#include "shadow_constants.h"

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    NATIVE_RESAMPLE_BRIDGE_OFF = 0,
    NATIVE_RESAMPLE_BRIDGE_MIX,
    NATIVE_RESAMPLE_BRIDGE_OVERWRITE
} native_resample_bridge_mode_t;

typedef enum {
    NATIVE_SAMPLER_SOURCE_UNKNOWN = 0,
    NATIVE_SAMPLER_SOURCE_RESAMPLING,
    NATIVE_SAMPLER_SOURCE_LINE_IN,
    NATIVE_SAMPLER_SOURCE_MIC_IN,
    NATIVE_SAMPLER_SOURCE_USB_C_IN
} native_sampler_source_t;

typedef struct {
    float rms_l;
    float rms_r;
    float rms_mid;
    float rms_side;
    float rms_low_l;
    float rms_low_r;
} native_audio_metrics_t;

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    unsigned char **global_mmap_addr;       /* Ptr to shim's global_mmap_addr */
    volatile float *shadow_master_volume;   /* Ptr to shim's shadow_master_volume */
} resample_host_t;

/* ============================================================================
 * Audio layout constants (from shim)
 * ============================================================================ */

#define RESAMPLE_AUDIO_IN_OFFSET  2304
#define RESAMPLE_AUDIO_BUFFER_SIZE 512  /* 128 frames * 2 channels * 2 bytes */

/* ============================================================================
 * Extern globals
 * ============================================================================ */

extern volatile native_resample_bridge_mode_t native_resample_bridge_mode;
extern volatile native_sampler_source_t native_sampler_source;
extern volatile native_sampler_source_t native_sampler_source_last_known;
extern volatile int link_audio_routing_enabled;

/* Snapshot and component buffers - written by shim rendering, read by apply */
extern int16_t native_total_mix_snapshot[];
extern volatile int native_total_mix_snapshot_valid;
extern int16_t native_bridge_move_component[];
extern int16_t native_bridge_me_component[];
extern float native_bridge_capture_mv;
extern volatile int native_bridge_split_valid;

/* Overwrite makeup diagnostics */
extern volatile float native_bridge_makeup_desired_gain;
extern volatile float native_bridge_makeup_applied_gain;
extern volatile int native_bridge_makeup_limited;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize resample bridge with host pointers. */
void resample_init(const resample_host_t *host);

/* Name helpers */
const char *native_resample_bridge_mode_name(native_resample_bridge_mode_t mode);
const char *native_sampler_source_name(native_sampler_source_t src);

/* Mode parsing */
native_resample_bridge_mode_t native_resample_bridge_mode_from_text(const char *text);
void native_resample_bridge_load_mode_from_shadow_config(void);

/* Source tracking (called from D-Bus text handler) */
void native_sampler_update_from_dbus_text(const char *text);

/* Snapshot capture (called from shim rendering) */
void native_capture_total_mix_snapshot_from_buffer(const int16_t *src);

/* Source gating policy */
int native_resample_bridge_source_allows_apply(native_resample_bridge_mode_t mode);

/* Apply bridge to AUDIO_IN (called from ioctl handler) */
void native_resample_bridge_apply(void);

/* Audio metrics computation */
void native_compute_audio_metrics(const int16_t *buf, native_audio_metrics_t *m);

#endif /* SHADOW_RESAMPLE_H */
