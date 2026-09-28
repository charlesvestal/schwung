/* move_model_sync.h -- keep Schwung aligned with Move's live set, from the model.
 *
 * move_model.h reads Move's document. This is the consumer that turns its
 * changes into Schwung state:
 *
 *   MUTE / SOLO. A slot follows its Move track. On a NEW DOCUMENT (a set
 *   load, or the first snapshot after boot) all four are taken as LEVELS; after
 *   that, only EDGES are applied, so Schwung's own slot-mute controls (slot
 *   settings, E16, CC map) still work between Move gestures, exactly as #540
 *   specified. This replaces the D-Bus "<name> muted" + gesture-window pairing
 *   and the Song.abl read, which remain only as the fallback for a firmware the
 *   model cannot resolve.
 *
 *   SET CHANGES. A set load replaces the document, which the model sees within
 *   one 20 ms tick; Move rewrites Settings.json's currentSongIndex within ~12 ms
 *   of the swap completing (measured). So the identity poll runs on the model's
 *   edge instead of waiting for the next 1.4 s scan.
 *
 *   ALIGNMENT. Two generations live in shadow_control_t: move_doc_gen (the
 *   document Move has loaded) and set_doc_gen (the document Schwung's per-set
 *   state corresponds to). They differ from a set load until Schwung has
 *   switched its state over, and the UI refuses to autosave while they do --
 *   before this, the ~3 s detection window let state land in the outgoing
 *   set's folder. A reload of the SAME set aligns in C; a different set aligns
 *   when the UI acknowledges its SET_CHANGED handling ("set_aligned").
 *
 * Everything here runs on the move_model reader thread (SCHED_OTHER) or the
 * shim worker, never the SPI callback. */
#pragma once
#include <stdint.h>
#include "shadow_constants.h"

void     move_model_sync_init(shadow_control_t **control);
int      move_model_sync_active(void);     /* the model is valid: it owns mute/solo */
uint32_t move_model_sync_gen(void);        /* current doc generation, 0 if not active */
/* 1 once the current generation has been stable long enough that a Settings.json
 * read is known to postdate Move's rewrite of it (see the .c). */
int      move_model_sync_settled(void);
/* A set load the model saw has not been aligned yet (move_doc_gen != set_doc_gen). */
int      move_model_sync_misaligned(void);

/* SPI thread: apply the mute/solo changes the reader posted. Called beside
 * shadow_set_pages_consume(), so every write to the slot mix flags happens on
 * the one thread that also serves slot:muted / slot:soloed. */
void     move_model_sync_apply_pending(void);
/* Shim worker, every tick: publish liveness to the UI (move_model_ready) and
 * expire a set misalignment nothing can resolve. */
void     move_model_sync_housekeep(void);
