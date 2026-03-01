/* shadow_overlay.h - Display overlay drawing and state sync
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_OVERLAY_H
#define SHADOW_OVERLAY_H

#include <stdint.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"
#include "plugin_api_v1.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Overlay knobs activation mode (from shadow_control->overlay_knobs_mode) */
#define OVERLAY_KNOBS_SHIFT     0
#define OVERLAY_KNOBS_JOG_TOUCH 1
#define OVERLAY_KNOBS_OFF       2
/* OVERLAY_KNOBS_NATIVE (3) defined in shadow_dbus.h */

#define SHIFT_KNOB_OVERLAY_FRAMES 60  /* ~1 second at 60fps */

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*announce)(const char *msg);          /* send_screenreader_announcement */
    shadow_control_t *volatile *shadow_control;
    shadow_overlay_state_t *volatile *shadow_overlay_shm;
    shadow_chain_slot_t *chain_slots;           /* shadow_chain_slots array */
    const plugin_api_v2_t *volatile *plugin_v2; /* shadow_plugin_v2 ptr */
} overlay_host_t;

/* ============================================================================
 * Extern globals - overlay state readable/writable by the shim
 * ============================================================================ */

extern int shift_knob_overlay_active;
extern int shift_knob_overlay_timeout;
extern int shift_knob_overlay_slot;
extern int shift_knob_overlay_knob;
extern char shift_knob_overlay_patch[64];
extern char shift_knob_overlay_param[64];
extern char shift_knob_overlay_value[32];

/* Font data */
extern const uint8_t overlay_font_5x7[96][7];

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize overlay module with host pointers. */
void overlay_init(const overlay_host_t *host);

/* Drawing primitives */
void overlay_draw_char(uint8_t *buf, int x, int y, char c, int color);
void overlay_draw_string(uint8_t *buf, int x, int y, const char *str, int color);
void overlay_fill_rect(uint8_t *buf, int x, int y, int w, int h, int color);

/* Draw the shift+knob overlay onto a display buffer */
void overlay_draw_shift_knob(uint8_t *buf);

/* Update overlay state when a knob CC is processed in Move mode with Shift held */
void shift_knob_update_overlay(int slot, int knob_num, uint8_t cc_value);

/* Read track mute states from Song.abl for the given set name. */
int shadow_read_set_mute_states(const char *set_name, int muted_out[4], int soloed_out[4]);

/* Blit a rectangular region from src onto dst (both 128x64 1bpp SSD1306) */
void overlay_blit_rect(uint8_t *dst, const uint8_t *src,
                       int rx, int ry, int rw, int rh);

/* Sync overlay state to shared memory for JS rendering */
void shadow_overlay_sync(void);

#endif /* SHADOW_OVERLAY_H */
