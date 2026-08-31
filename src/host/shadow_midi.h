/* shadow_midi.h - MIDI routing, dispatch, and forwarding
 * Extracted from schwung_shim.c for maintainability. */

#ifndef SHADOW_MIDI_H
#define SHADOW_MIDI_H

#include <stdint.h>
#include "shadow_constants.h"
#include "shadow_chain_types.h"
#include "plugin_api_v1.h"

/* ============================================================================
 * Move Hardware CC Constants
 * ============================================================================ */

#ifndef CC_SHIFT
#define CC_SHIFT 49
#define CC_JOG_CLICK 3
#define CC_JOG_WHEEL 14
#define CC_BACK 51
#define CC_MASTER_KNOB 79
#define CC_UP 55
#define CC_DOWN 54
#define CC_MENU 50
#define CC_CAPTURE 52
#define CC_UNDO 56
#define CC_LOOP 58
#define CC_COPY 60
#define CC_LEFT 62
#define CC_RIGHT 63
#define CC_KNOB1 71
#define CC_KNOB2 72
#define CC_KNOB3 73
#define CC_KNOB4 74
#define CC_KNOB5 75
#define CC_KNOB6 76
#define CC_KNOB7 77
#define CC_KNOB8 78
#define CC_PLAY 85
#define CC_REC 86
#define CC_SAMPLE 87
#define CC_MUTE 88
#define CC_MIC_IN_DETECT 114
#define CC_LINE_OUT_DETECT 115
#define CC_RECORD 118
#define CC_DELETE 119
#define CC_STEP_UI_FIRST 16
#define CC_STEP_UI_LAST 31
#endif

/* ============================================================================
 * Mailbox Layout Constants
 * ============================================================================ */

#ifndef MIDI_OUT_OFFSET
#define MAILBOX_SIZE 4096
#define MIDI_OUT_OFFSET 0
#define AUDIO_OUT_OFFSET 256
#define DISPLAY_OFFSET 768
#define MIDI_IN_OFFSET 2048
#define AUDIO_IN_OFFSET 2304
#define AUDIO_BUFFER_SIZE 512
#endif

/* ============================================================================
 * Callback struct
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    void (*midi_out_logf)(const char *fmt, ...);
    int (*midi_out_log_enabled)(void);
    void (*ui_state_update_slot)(int slot);
    void (*master_fx_forward_midi)(const uint8_t *msg, int len, int source);
    void (*queue_led)(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2);
    void (*init_led_queue)(void);
    /* Shared state */
    shadow_chain_slot_t *chain_slots;
    const plugin_api_v2_t *volatile *plugin_v2;
    shadow_control_t *volatile *shadow_control;
    unsigned char **global_mmap_addr;
    int *shadow_inprocess_ready;
    uint8_t *shadow_display_mode;
    /* SHM segment pointers */
    uint8_t **shadow_midi_shm;
    shadow_midi_out_t **shadow_midi_out_shm;
    uint8_t **shadow_ui_midi_shm;
    shadow_midi_dsp_t **shadow_midi_dsp_shm;
    shadow_midi_inject_t **shadow_midi_inject_shm;
    shadow_midi_inject_t **shadow_midi_inject_ui_shm;
    uint8_t *shadow_mailbox;
    /* No Master FX capture pointer here on purpose: it used to be
     * a pointer to shadow_master_fx_slots[0].capture, cached at init into a
     * raw pointer aimed at an array whose contents now permute. Master FX capture
     * is read per event from the live array instead — see
     * shadow_focused_captures_note/cc below. */
    /* Per-slot idle tracking */
    int *slot_idle;
    int *slot_silence_frames;
    int *slot_fx_idle;
    int *slot_fx_silence_frames;
} midi_host_t;

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize MIDI routing module with host pointers. */
void midi_routing_init(const midi_host_t *host);

/* Remap MIDI channel for a specific slot based on forward_channel config. */
uint8_t shadow_chain_remap_channel(int slot, uint8_t status);

/* Dispatch a USB-MIDI packet to matching chain slots (by channel).
 * When skip_direct is 1, slots with receive=All and forward=THRU are skipped
 * (they receive MIDI via the direct MIDI_IN path instead). */
void shadow_chain_dispatch_midi_to_slots(const uint8_t *pkt, int log_on, int *midi_log_count, int skip_direct);

/* Deliver one cable-2 SysEx packet to slots whose module declared
 * capabilities.wants_sysex. Fragments, not reassembled messages.
 *
 * Takes an 8-BYTE MIDI_IN SLOT, not a 4-byte packet: it dedups on the packet
 * plus the XMOS timestamp that follows it, so a 4-byte buffer would read past
 * the end. Callers walk MIDI_IN at the 8-byte stride and pass &in_src[i]. */
void shadow_chain_dispatch_sysex_to_slots(const uint8_t *slot8);

/* Broadcast a 1-byte system-realtime message (0xF8/0xFA/0xFB/0xFC) to every
 * active chain slot, bypassing per-slot channel remap (which would corrupt the
 * status byte). For internally generated transport clock. */
void shadow_chain_broadcast_realtime(uint8_t status);

/* Dispatch external MIDI from MIDI_IN cable 2 directly to slots configured
 * for passthrough (receive=All, forward=THRU).  Bypasses Move's MIDI_OUT
 * channel remapping so that MPE per-note expression data stays on the
 * correct channels. */
void shadow_dispatch_direct_external_midi(void);

/* Dispatch cable-2 (USB-A external MIDI) note/voice messages to chain slots
 * matched by configured receive channel.  Used when no tool module is active
 * so that Schwung instruments respond to external MIDI by channel. */
void shadow_dispatch_cable2_channeled_slots(void);

/* External cable-2 dispatch ring.  Used by shadow_inprocess_process_midi to
 * detect MIDI_OUT echoes of external cable-2 events that were already
 * dispatched to chain slots via the MIDI_IN path.  The MIDI_IN scan alone is
 * unreliable under chord bursts because Move can consume or reuse the cable-2
 * slot between the dispatch frame and the echo frame, leaving no signature
 * for the echo check to find.  Recording at dispatch time gives a stable
 * cross-frame signal.  Tick the ring once per SPI frame so stale entries age
 * out within a small window. */
void shadow_external_dispatch_tick(void);
void shadow_external_dispatch_record(uint8_t status, uint8_t d1, uint8_t d2);
int  shadow_external_dispatch_was_recent(uint8_t status, uint8_t d1, uint8_t d2);

/* Forward CC/pitch bend/aftertouch from external MIDI (cable 2) into MIDI_OUT. */
void shadow_forward_external_cc_to_out(void);

/* Inject shadow UI MIDI output into the mailbox before ioctl. */
void shadow_inject_ui_midi_out(void);

/* Drain MIDI-to-DSP buffer from shadow UI and dispatch to chain slots. */
void shadow_drain_ui_midi_dsp(void);

/* Drain MIDI inject buffer into Move's MIDI_IN (post-ioctl). */
void shadow_drain_midi_inject(void);

/* Queue a 4-byte USB-MIDI packet for Move's firmware only, bypassing the
 * test-bus inject ring (which is owned by the overtake publisher whenever a
 * module is up). Single slot; the only producer is the overtake transition.
 * Delivered by shadow_drain_midi_inject on the next frame where MIDI_IN is
 * idle, and never routed to an overtake module. */
void shadow_queue_packet_to_move(const uint8_t pkt[4]);

/* Queue a 4-byte USB-MIDI packet for MIDI_IN injection (Pre-mode MIDI FX).
 * The cable nibble in msg[0] is preserved by the drain. Callers choose:
 *   cable 0 → internal hardware (Move treats as pad/button input)
 *   cable 2 → external USB MIDI (routed by channel to Move's track synths)
 * See shadow_midi.c for the full drain semantics.
 * Returns 4 on success, 0 if SHM unavailable or ring full. */
int shadow_chain_midi_inject(const uint8_t *msg, int len);

/* Copy incoming MIDI from mailbox to shadow shared memory. */
void shadow_forward_midi(void);

/* Does the focused target capture this control? (0-3 = chain slot, 4 = Master
 * FX, in which case it is a union over every loaded Master FX position, so a
 * capturing module keeps its MIDI wherever a move gesture puts it.) */
int shadow_focused_captures_note(uint8_t note);
int shadow_focused_captures_cc(uint8_t cc);

#endif /* SHADOW_MIDI_H */
