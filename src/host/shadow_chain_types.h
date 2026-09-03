/* shadow_chain_types.h - Shared chain slot and capture rule types
 * Extracted from schwung_shim.c so multiple modules can use them. */

#ifndef SHADOW_CHAIN_TYPES_H
#define SHADOW_CHAIN_TYPES_H

#include <stdint.h>

/* Capture rules: bitmaps for which notes/CCs a slot captures */
typedef struct shadow_capture_rules_t {
    uint8_t notes[16];   /* bitmap: 128 notes, 16 bytes */
    uint8_t ccs[16];     /* bitmap: 128 CCs, 16 bytes */
} shadow_capture_rules_t;

/* Fade envelope for seamless patch transitions */
typedef struct slot_fade_t {
    float gain;            /* current gain 0.0-1.0 */
    float target;          /* target gain: 0.0 (fading out) or 1.0 (fading in) */
    float step;            /* per-sample gain change (1.0/FADE_SAMPLES) */
    int pending_patch;     /* patch index to load after fade-out (-1 = none) */
    uint8_t pending_clear; /* tear down DSP after fade-out completes */
} slot_fade_t;

/* 50ms fade at 44100Hz */
#define SLOT_FADE_SAMPLES 2205
#define SLOT_FADE_STEP (1.0f / SLOT_FADE_SAMPLES)

/* forward_channel sentinel values */
#define SHADOW_FORWARD_THRU (-2)  /* passthrough: preserve original MIDI channel */
#define SHADOW_FORWARD_AUTO (-1)  /* auto: ask the module, else the receive channel */

typedef struct shadow_chain_slot_t {
    void *instance;
    int channel;
    int patch_index;
    int active;
    float volume;           /* 0.0 to 1.0, user-set level (never modified by mute/solo) */
    int muted;              /* 1 = muted (Mute+Track or Move speakerOn sync) */
    int soloed;             /* 1 = soloed (Shift+Mute+Track or Move solo-cue sync) */
    int feedback_hold;      /* 1 = booted muted as a line-input feedback guard; JS clears once jack state is safe */
    int forward_channel;    /* -2 = passthrough, -1 = auto, 0-15 = forward MIDI to this channel */
    /* What the loaded synth declared as capabilities.default_forward_channel,
     * cached at load: -2 = THRU, 0-15 = channel, -1 = the module said nothing.
     * AUTO consults this before falling back to the receive channel, so "Auto"
     * keeps meaning Auto instead of being silently rewritten to a fixed channel
     * the first time a load path happens to run. Cached, never queried from
     * shadow_chain_remap_channel -- that runs per MIDI event. */
    int default_forward_channel;
    int transpose;          /* semitone offset applied to incoming note-on/off/poly-AT, range -12..+12 */
    /* 1 = some component in this slot declared capabilities.wants_sysex, so
     * cable-2 SysEx fragments are delivered to it. Read from the chain host at
     * load; slots that did not ask see no SysEx at all, which is the behaviour
     * every slot had before. */
    int wants_sysex;
    char patch_name[64];
    shadow_capture_rules_t capture;  /* MIDI controls this slot captures when focused */
    /* Rules the loaded SYNTH MODULE declares for itself (module.json
     * capabilities.capture), kept apart from the patch's so a module swap can
     * re-derive one without losing the other. A slot captures the union.
     * Re-derived on every load path — including the autosave restore, which
     * never opened a patch file and so left a restored slot with no rules at
     * all. Cached by module id so the file is read once per load, not per
     * set_param. */
    shadow_capture_rules_t module_capture;
    char module_capture_id[64];
    slot_fade_t fade;                /* fade envelope for seamless transitions */
} shadow_chain_slot_t;

#endif /* SHADOW_CHAIN_TYPES_H */
