/* The step menu's device glue (step_menu.c). step_menu.h is the pure half. */
#ifndef STEP_MENU_GLUE_H
#define STEP_MENU_GLUE_H

#include <stdint.h>
#include "move_model.h"
#include "shadow_constants.h"

/* MODEL THREAD, every tick: the edited clip's notes around the shown page. */
void step_menu_publish_page(const move_model_t *m, const mm_note_t *notes, int n,
                            const mm_clip_ref_t *ref);
/* SPI CALLBACK, per cable-0 MIDI_IN event: SM_PASS / SM_SWALLOW / SM_REWRITE
 * (then `out` holds the replacement bytes). */
int step_menu_on_input(uint8_t status, uint8_t d1, uint8_t d2, uint8_t out[3],
                       uint32_t held_mask, int shift_held, int eligible, uint64_t now_ms);
/* SPI CALLBACK, after compaction: steps whose withheld release is now due. */
uint32_t step_menu_take_due_releases(uint64_t now_ms);
/* The step the open menu is about, or -1 when it is closed. */
int step_menu_open_step(void);
/* A release the shim took from Move (the hand-off) and owes it at due_ms. */
void step_menu_owe_release(int step, uint64_t due_ms);
/* SPI CALLBACK, once per frame after the scan. */
void step_menu_frame(shadow_control_t *ctl, uint32_t held_mask, int eligible);

#endif
