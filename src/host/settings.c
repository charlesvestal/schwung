/*
 * Host Settings - Persistent user preferences for MIDI behavior
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/* Velocity curve names for display and parsing */
static const char *velocity_curve_names[] = {
    "linear",
    "soft",
    "hard",
    "full"
};

/* Clock mode names for display and parsing */
static const char *clock_mode_names[] = {
    "off",
    "internal",
    "external"
};

/* Pad layout names for display and parsing */
static const char *pad_layout_names[] = {
    "chromatic",
    "fourth"
};

void settings_init(host_settings_t *s) {
    s->velocity_curve = VELOCITY_CURVE_LINEAR;
    s->aftertouch_enabled = 1;
    s->aftertouch_deadzone = 0;
    s->pad_layout = PAD_LAYOUT_CHROMATIC;
    s->clock_mode = CLOCK_MODE_INTERNAL;
    s->tempo_bpm = 120;
}

const char* settings_velocity_curve_name(velocity_curve_t curve) {
    if (curve >= 0 && curve < VELOCITY_CURVE_COUNT) {
        return velocity_curve_names[curve];
    }
    return "linear";
}

velocity_curve_t settings_parse_velocity_curve(const char *str) {
    if (!str) return VELOCITY_CURVE_LINEAR;

    for (int i = 0; i < VELOCITY_CURVE_COUNT; i++) {
        if (strcmp(str, velocity_curve_names[i]) == 0) {
            return (velocity_curve_t)i;
        }
    }
    return VELOCITY_CURVE_LINEAR;
}

const char* settings_pad_layout_name(pad_layout_t layout) {
    if (layout >= 0 && layout < PAD_LAYOUT_COUNT) {
        return pad_layout_names[layout];
    }
    return "chromatic";
}

pad_layout_t settings_parse_pad_layout(const char *str) {
    if (!str) return PAD_LAYOUT_CHROMATIC;

    for (int i = 0; i < PAD_LAYOUT_COUNT; i++) {
        if (strcmp(str, pad_layout_names[i]) == 0) {
            return (pad_layout_t)i;
        }
    }
    return PAD_LAYOUT_CHROMATIC;
}

void settings_load(host_settings_t *s, const char *path) {
    settings_init(s);

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("settings: no settings file, using defaults\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Remove newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Parse key=value */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "velocity_curve") == 0) {
            s->velocity_curve = settings_parse_velocity_curve(val);
        } else if (strcmp(key, "aftertouch_enabled") == 0) {
            s->aftertouch_enabled = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "aftertouch_deadzone") == 0) {
            int dz = atoi(val);
            if (dz < 0) dz = 0;
            if (dz > 50) dz = 50;
            s->aftertouch_deadzone = dz;
        } else if (strcmp(key, "pad_layout") == 0) {
            s->pad_layout = settings_parse_pad_layout(val);
        } else if (strcmp(key, "clock_mode") == 0) {
            for (int i = 0; i < CLOCK_MODE_COUNT; i++) {
                if (strcmp(val, clock_mode_names[i]) == 0) {
                    s->clock_mode = (clock_mode_t)i;
                    break;
                }
            }
        } else if (strcmp(key, "tempo_bpm") == 0) {
            int bpm = atoi(val);
            if (bpm < 20) bpm = 20;
            if (bpm > 300) bpm = 300;
            s->tempo_bpm = bpm;
        }
    }

    fclose(f);
    printf("settings: loaded velocity_curve=%s aftertouch=%s deadzone=%d pad_layout=%s clock=%s tempo=%d\n",
           settings_velocity_curve_name(s->velocity_curve),
           s->aftertouch_enabled ? "on" : "off",
           s->aftertouch_deadzone,
           settings_pad_layout_name(s->pad_layout),
           clock_mode_names[s->clock_mode],
           s->tempo_bpm);
}

int settings_save(const host_settings_t *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("settings: cannot write to %s\n", path);
        return -1;
    }

    fprintf(f, "velocity_curve=%s\n", settings_velocity_curve_name(s->velocity_curve));
    fprintf(f, "aftertouch_enabled=%d\n", s->aftertouch_enabled);
    fprintf(f, "aftertouch_deadzone=%d\n", s->aftertouch_deadzone);
    fprintf(f, "pad_layout=%s\n", settings_pad_layout_name(s->pad_layout));
    fprintf(f, "clock_mode=%s\n", clock_mode_names[s->clock_mode]);
    fprintf(f, "tempo_bpm=%d\n", s->tempo_bpm);

    fclose(f);
    printf("settings: saved to %s\n", path);
    return 0;
}

uint8_t settings_apply_velocity(const host_settings_t *s, uint8_t velocity) {
    if (velocity == 0) return 0;  /* Don't transform Note Off */

    switch (s->velocity_curve) {
        case VELOCITY_CURVE_LINEAR:
            return velocity;

        case VELOCITY_CURVE_SOFT:
            /* Boost low velocities: 1→64, 127→127 */
            return 64 + (velocity / 2);

        case VELOCITY_CURVE_HARD:
            /* Exponential curve - requires firm press */
            return (uint8_t)((velocity * velocity) / 127);

        case VELOCITY_CURVE_FULL:
            return 127;

        default:
            return velocity;
    }
}

int settings_apply_aftertouch(const host_settings_t *s, uint8_t *value) {
    if (!s->aftertouch_enabled) {
        return 0;  /* Drop message */
    }

    if (*value < s->aftertouch_deadzone) {
        *value = 0;
    }

    return 1;  /* Forward message */
}
