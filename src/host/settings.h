/*
 * Host Settings - Persistent user preferences for MIDI behavior
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

/* Velocity curve options */
typedef enum {
    VELOCITY_CURVE_LINEAR = 0,
    VELOCITY_CURVE_SOFT,
    VELOCITY_CURVE_HARD,
    VELOCITY_CURVE_FULL,
    VELOCITY_CURVE_COUNT
} velocity_curve_t;

/* MIDI clock mode options */
typedef enum {
    CLOCK_MODE_OFF = 0,
    CLOCK_MODE_INTERNAL,
    CLOCK_MODE_EXTERNAL,
    CLOCK_MODE_COUNT
} clock_mode_t;

/* Host settings structure */
typedef struct {
    velocity_curve_t velocity_curve;
    int aftertouch_enabled;
    int aftertouch_deadzone;  /* 0-50 */
    clock_mode_t clock_mode;  /* off/internal/external */
    int tempo_bpm;            /* 20-300 BPM */
} host_settings_t;

/* Default settings path */
#define SETTINGS_PATH "/data/UserData/move-anything/settings.txt"

/* Initialize settings to defaults */
void settings_init(host_settings_t *s);

/* Load settings from file (missing values use defaults) */
void settings_load(host_settings_t *s, const char *path);

/* Save settings to file */
int settings_save(const host_settings_t *s, const char *path);

/* Apply velocity curve transform, returns transformed velocity */
uint8_t settings_apply_velocity(const host_settings_t *s, uint8_t velocity);

/* Apply aftertouch transform
 * Modifies value in place
 * Returns 1 if message should be forwarded, 0 if it should be dropped */
int settings_apply_aftertouch(const host_settings_t *s, uint8_t *value);

/* Get velocity curve name for display */
const char* settings_velocity_curve_name(velocity_curve_t curve);

/* Parse velocity curve from string */
velocity_curve_t settings_parse_velocity_curve(const char *str);

#endif /* SETTINGS_H */
