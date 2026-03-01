/* shadow_midi.c - MIDI routing, dispatch, and forwarding
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shadow_midi.h"
#include "shadow_chain_mgmt.h"
#include "shadow_led_queue.h"

/* ============================================================================
 * Host callbacks (set by midi_routing_init)
 * ============================================================================ */

static void (*host_log)(const char *msg);
static void (*host_midi_out_logf)(const char *fmt, ...);
static int (*host_midi_out_log_enabled)(void);
static void (*host_ui_state_update_slot)(int slot);
static void (*host_master_fx_forward_midi)(const uint8_t *msg, int len, int source);
static void (*host_queue_led)(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2);
static void (*host_init_led_queue)(void);

/* Shared state pointers */
static shadow_chain_slot_t *host_chain_slots;
static const plugin_api_v2_t *volatile *host_plugin_v2;
static shadow_control_t *volatile *host_shadow_control;
static unsigned char **host_global_mmap_addr;
static int *host_shadow_inprocess_ready;
static uint8_t *host_shadow_display_mode;

/* SHM segment pointers */
static uint8_t **host_shadow_midi_shm;
static shadow_midi_out_t **host_shadow_midi_out_shm;
static uint8_t **host_shadow_ui_midi_shm;
static shadow_midi_dsp_t **host_shadow_midi_dsp_shm;
static uint8_t *host_shadow_mailbox;

/* Capture */
static shadow_capture_rules_t *host_master_fx_capture;

/* Idle tracking */
static int *host_slot_idle;
static int *host_slot_silence_frames;
static int *host_slot_fx_idle;
static int *host_slot_fx_silence_frames;

void midi_routing_init(const midi_host_t *host)
{
    host_log = host->log;
    host_midi_out_logf = host->midi_out_logf;
    host_midi_out_log_enabled = host->midi_out_log_enabled;
    host_ui_state_update_slot = host->ui_state_update_slot;
    host_master_fx_forward_midi = host->master_fx_forward_midi;
    host_queue_led = host->queue_led;
    host_init_led_queue = host->init_led_queue;
    host_chain_slots = host->chain_slots;
    host_plugin_v2 = host->plugin_v2;
    host_shadow_control = host->shadow_control;
    host_global_mmap_addr = host->global_mmap_addr;
    host_shadow_inprocess_ready = host->shadow_inprocess_ready;
    host_shadow_display_mode = host->shadow_display_mode;
    host_shadow_midi_shm = host->shadow_midi_shm;
    host_shadow_midi_out_shm = host->shadow_midi_out_shm;
    host_shadow_ui_midi_shm = host->shadow_ui_midi_shm;
    host_shadow_midi_dsp_shm = host->shadow_midi_dsp_shm;
    host_shadow_mailbox = host->shadow_mailbox;
    host_master_fx_capture = host->master_fx_capture;
    host_slot_idle = host->slot_idle;
    host_slot_silence_frames = host->slot_silence_frames;
    host_slot_fx_idle = host->slot_fx_idle;
    host_slot_fx_silence_frames = host->slot_fx_silence_frames;
}

/* ============================================================================
 * Channel remapping
 * ============================================================================ */

/* Apply forward channel remapping for a slot.
 * If forward_channel >= 0, remap to that specific channel.
 * If forward_channel == -1 (auto), use the slot's receive channel. */
uint8_t shadow_chain_remap_channel(int slot, uint8_t status)
{
    int fwd_ch = host_chain_slots[slot].forward_channel;
    if (fwd_ch == -2) {
        /* Passthrough: preserve original MIDI channel */
        return status;
    }
    if (fwd_ch >= 0 && fwd_ch <= 15) {
        /* Specific forward channel */
        return (status & 0xF0) | (uint8_t)fwd_ch;
    }
    /* Auto (-1): use the receive channel, but if recv=All (-1), passthrough */
    if (host_chain_slots[slot].channel < 0) {
        return status;  /* Recv=All + Fwd=Auto → passthrough */
    }
    return (status & 0xF0) | (uint8_t)host_chain_slots[slot].channel;
}

/* ============================================================================
 * MIDI dispatch to chain slots
 * ============================================================================ */

/* Dispatch MIDI to all matching slots (supports recv=All broadcasting) */
void shadow_chain_dispatch_midi_to_slots(const uint8_t *pkt, int log_on, int *midi_log_count)
{
    const plugin_api_v2_t *pv2 = *host_plugin_v2;
    uint8_t status_usb = pkt[1];
    uint8_t type = status_usb & 0xF0;
    uint8_t midi_ch = status_usb & 0x0F;
    uint8_t note = pkt[2];
    int dispatched = 0;

    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        /* Check channel match: slot receives this channel, or slot is set to All (-1) */
        if (host_chain_slots[i].channel != (int)midi_ch && host_chain_slots[i].channel != -1)
            continue;

        /* Lazy activation check */
        if (!host_chain_slots[i].active) {
            if (pv2 && pv2->get_param &&
                host_chain_slots[i].instance) {
                char buf[64];
                int len = pv2->get_param(host_chain_slots[i].instance,
                                          "synth_module", buf, sizeof(buf));
                if (len > 0) {
                    if (len < (int)sizeof(buf)) buf[len] = '\0';
                    else buf[sizeof(buf) - 1] = '\0';
                    if (buf[0] != '\0') {
                        host_chain_slots[i].active = 1;
                        if (host_ui_state_update_slot)
                            host_ui_state_update_slot(i);
                    }
                }
            }
            if (!host_chain_slots[i].active) continue;
        }

        /* Wake slot from idle on any MIDI dispatch */
        if (host_slot_idle[i] || host_slot_fx_idle[i]) {
            host_slot_idle[i] = 0;
            host_slot_silence_frames[i] = 0;
            host_slot_fx_idle[i] = 0;
            host_slot_fx_silence_frames[i] = 0;
        }

        /* Send MIDI to this slot */
        if (pv2 && pv2->on_midi) {
            uint8_t msg[3] = { shadow_chain_remap_channel(i, pkt[1]), pkt[2], pkt[3] };
            pv2->on_midi(host_chain_slots[i].instance, msg, 3,
                         MOVE_MIDI_SOURCE_EXTERNAL);
        }
        dispatched++;
    }

    /* Broadcast MIDI to ALL active slots for audio FX (e.g. ducker).
     * FX_BROADCAST only forwards to audio FX, not synth/MIDI FX, so this
     * is safe even for slots that already received normal MIDI dispatch. */
    if (pv2 && pv2->on_midi) {
        for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
            if (!host_chain_slots[i].active || !host_chain_slots[i].instance)
                continue;
            uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
            pv2->on_midi(host_chain_slots[i].instance, msg, 3,
                         MOVE_MIDI_SOURCE_FX_BROADCAST);
        }
    }

    /* Forward MIDI to master FX (e.g. ducker) regardless of slot routing */
    {
        uint8_t msg[3] = { pkt[1], pkt[2], pkt[3] };
        if (host_master_fx_forward_midi)
            host_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
    }

    if (log_on && type == 0x90 && pkt[3] > 0 && *midi_log_count < 100) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
            "midi_out: note=%u vel=%u ch=%u dispatched=%d",
            note, pkt[3], midi_ch, dispatched);
        if (host_log) host_log(dbg);
        if (host_midi_out_logf)
            host_midi_out_logf("midi_out: note=%u vel=%u ch=%u dispatched=%d",
                note, pkt[3], midi_ch, dispatched);
        (*midi_log_count)++;
    }
}

/* ============================================================================
 * External MIDI CC forwarding
 * ============================================================================ */

/* Forward CC, pitch bend, aftertouch from external MIDI (MIDI_IN cable 2) to MIDI_OUT.
 * Move echoes notes but not these message types, so we inject them into MIDI_OUT
 * so the DSP routing can pick them up alongside the echoed notes. */
void shadow_forward_external_cc_to_out(void)
{
    if (!*host_shadow_inprocess_ready || !*host_global_mmap_addr) return;

    uint8_t *in_src = *host_global_mmap_addr + MIDI_IN_OFFSET;
    uint8_t *out_dst = *host_global_mmap_addr + MIDI_OUT_OFFSET;

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = in_src[i] & 0x0F;
        uint8_t cable = (in_src[i] >> 4) & 0x0F;

        /* Only process external MIDI (cable 2) */
        if (cable != 0x02) continue;
        if (cin < 0x08 || cin > 0x0E) continue;

        uint8_t status = in_src[i + 1];
        uint8_t type = status & 0xF0;

        /* Only forward CC (0xB0), pitch bend (0xE0), channel aftertouch (0xD0), poly aftertouch (0xA0) */
        if (type != 0xB0 && type != 0xE0 && type != 0xD0 && type != 0xA0) continue;

        /* Find an empty slot in MIDI_OUT and inject the message */
        for (int j = 0; j < MIDI_BUFFER_SIZE; j += 4) {
            if (out_dst[j] == 0 && out_dst[j+1] == 0 && out_dst[j+2] == 0 && out_dst[j+3] == 0) {
                /* Copy the packet, keeping cable 2 */
                out_dst[j] = in_src[i];
                out_dst[j + 1] = in_src[i + 1];
                out_dst[j + 2] = in_src[i + 2];
                out_dst[j + 3] = in_src[i + 3];
                break;
            }
        }
    }
}

/* ============================================================================
 * Shadow UI MIDI inject/drain
 * ============================================================================ */

/* Inject shadow UI MIDI out into mailbox before ioctl. */
void shadow_inject_ui_midi_out(void)
{
    shadow_midi_out_t *midi_out_shm = *host_shadow_midi_out_shm;
    static uint8_t last_ready = 0;

    if (!midi_out_shm) return;
    if (midi_out_shm->ready == last_ready) return;

    last_ready = midi_out_shm->ready;
    if (host_init_led_queue) host_init_led_queue();

    /* Snapshot buffer first, then reset write_idx.
     * Copy before resetting to avoid a race where the JS process writes
     * new data between our reset and memcpy. */
    int snapshot_len = midi_out_shm->write_idx;
    uint8_t local_buf[SHADOW_MIDI_OUT_BUFFER_SIZE];
    int copy_len = snapshot_len < (int)SHADOW_MIDI_OUT_BUFFER_SIZE
                 ? snapshot_len : (int)SHADOW_MIDI_OUT_BUFFER_SIZE;
    if (copy_len > 0) {
        memcpy(local_buf, midi_out_shm->buffer, copy_len);
    }
    __sync_synchronize();
    midi_out_shm->write_idx = 0;
    memset(midi_out_shm->buffer, 0, SHADOW_MIDI_OUT_BUFFER_SIZE);

    /* Inject into shadow_mailbox at MIDI_OUT_OFFSET */
    uint8_t *midi_out = host_shadow_mailbox + MIDI_OUT_OFFSET;

    int hw_offset = 0;
    for (int i = 0; i < copy_len; i += 4) {
        uint8_t cin = local_buf[i];
        uint8_t cable = (cin >> 4) & 0x0F;
        uint8_t status = local_buf[i + 1];
        uint8_t data1 = local_buf[i + 2];
        uint8_t data2 = local_buf[i + 3];
        uint8_t type = status & 0xF0;

        /* Queue cable 0 LED messages (note-on, CC) for rate-limited sending */
        if (cable == 0 && (type == 0x90 || type == 0xB0)) {
            if (host_queue_led) host_queue_led(cin, status, data1, data2);
            continue;  /* Don't copy directly, will be flushed later */
        }

        /* All other messages: copy directly to mailbox */
        while (hw_offset < MIDI_BUFFER_SIZE) {
            if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= MIDI_BUFFER_SIZE) break;  /* Buffer full */

        memcpy(&midi_out[hw_offset], &local_buf[i], 4);
        hw_offset += 4;
    }
}

/* Drain MIDI-to-DSP buffer from shadow UI and dispatch to chain slots. */
void shadow_drain_ui_midi_dsp(void)
{
    shadow_midi_dsp_t *midi_dsp_shm = *host_shadow_midi_dsp_shm;
    static uint8_t last_ready = 0;

    if (!midi_dsp_shm) return;
    if (midi_dsp_shm->ready == last_ready) return;

    last_ready = midi_dsp_shm->ready;

    static int midi_log_count = 0;
    int log_on = host_midi_out_log_enabled ? host_midi_out_log_enabled() : 0;

    for (int i = 0; i < midi_dsp_shm->write_idx && i < SHADOW_MIDI_DSP_BUFFER_SIZE; i += 4) {
        uint8_t status = midi_dsp_shm->buffer[i];
        uint8_t d1 = midi_dsp_shm->buffer[i + 1];
        uint8_t d2 = midi_dsp_shm->buffer[i + 2];

        /* Validate status byte has high bit set */
        if (!(status & 0x80)) continue;

        /* Construct USB-MIDI packet for dispatch: [CIN, status, d1, d2] */
        uint8_t cin = (status >> 4) & 0x0F;
        uint8_t pkt[4] = { cin, status, d1, d2 };

        shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count);
    }

    /* Clear after processing */
    midi_dsp_shm->write_idx = 0;
    memset(midi_dsp_shm->buffer, 0, SHADOW_MIDI_DSP_BUFFER_SIZE);
}

/* ============================================================================
 * MIDI forwarding to shadow shared memory
 * ============================================================================ */

/* Copy incoming MIDI from mailbox to shadow shared memory */
void shadow_forward_midi(void)
{
    uint8_t *shadow_midi_shm = *host_shadow_midi_shm;
    unsigned char *global_mmap_addr = *host_global_mmap_addr;
    shadow_control_t *shadow_control = *host_shadow_control;

    if (!shadow_midi_shm || !global_mmap_addr) return;
    if (!shadow_control) return;

    /* Cache flag file checks - re-check frequently so debug flags take effect quickly. */
    static int cache_counter = 0;
    static int cached_ch3_only = 0;
    static int cached_block_ch1 = 0;
    static int cached_allow_ch5_8 = 0;
    static int cached_notes_only = 0;
    static int cached_allow_cable0 = 0;
    static int cached_drop_cable_f = 0;
    static int cached_log_on = 0;
    static int cached_drop_ui = 0;
    static int cache_initialized = 0;

    /* Only check on first call and then every 200 calls */
    if (!cache_initialized || (cache_counter++ % 200 == 0)) {
        cache_initialized = 1;
        cached_ch3_only = (access("/data/UserData/move-anything/shadow_midi_ch3_only", F_OK) == 0);
        cached_block_ch1 = (access("/data/UserData/move-anything/shadow_midi_block_ch1", F_OK) == 0);
        cached_allow_ch5_8 = (access("/data/UserData/move-anything/shadow_midi_allow_ch5_8", F_OK) == 0);
        cached_notes_only = (access("/data/UserData/move-anything/shadow_midi_notes_only", F_OK) == 0);
        cached_allow_cable0 = (access("/data/UserData/move-anything/shadow_midi_allow_cable0", F_OK) == 0);
        cached_drop_cable_f = (access("/data/UserData/move-anything/shadow_midi_drop_cable_f", F_OK) == 0);
        cached_log_on = (access("/data/UserData/move-anything/shadow_midi_log_on", F_OK) == 0);
        cached_drop_ui = (access("/data/UserData/move-anything/shadow_midi_drop_ui", F_OK) == 0);
    }

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
    int ch3_only = cached_ch3_only;
    int block_ch1 = cached_block_ch1;
    int allow_ch5_8 = cached_allow_ch5_8;
    int notes_only = cached_notes_only;
    int allow_cable0 = cached_allow_cable0;
    int drop_cable_f = cached_drop_cable_f;
    int log_on = cached_log_on;
    int drop_ui = cached_drop_ui;
    static FILE *log = NULL;

    /* Only copy if there's actual MIDI data (check first 64 bytes for non-zero) */
    int has_midi = 0;
    uint8_t filtered[MIDI_BUFFER_SIZE];
    memset(filtered, 0, sizeof(filtered));

    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        uint8_t cin = src[i] & 0x0F;
        uint8_t cable = (src[i] >> 4) & 0x0F;
        if (cin < 0x08 || cin > 0x0E) {
            continue;
        }
        if (allow_cable0 && cable != 0x00) {
            continue;
        }
        if (drop_cable_f && cable == 0x0F) {
            continue;
        }
        uint8_t status = src[i + 1];
        if (cable == 0x00) {
            uint8_t type = status & 0xF0;
            if (drop_ui) {
                if ((type == 0x90 || type == 0x80) && src[i + 2] < 10) {
                    continue; /* Filter knob-touch notes from internal MIDI */
                }
                if (type == 0xB0) {
                    uint8_t cc = src[i + 2];
                    if ((cc >= CC_STEP_UI_FIRST && cc <= CC_STEP_UI_LAST) ||
                        cc == CC_SHIFT || cc == CC_JOG_CLICK || cc == CC_BACK ||
                        cc == CC_MENU || cc == CC_CAPTURE || cc == CC_UP ||
                        cc == CC_DOWN || cc == CC_UNDO || cc == CC_LOOP ||
                        cc == CC_COPY || cc == CC_LEFT || cc == CC_RIGHT ||
                        cc == CC_KNOB1 || cc == CC_KNOB2 || cc == CC_KNOB3 ||
                        cc == CC_KNOB4 || cc == CC_KNOB5 || cc == CC_KNOB6 ||
                        cc == CC_KNOB7 || cc == CC_KNOB8 || cc == CC_MASTER_KNOB ||
                        cc == CC_PLAY || cc == CC_REC || cc == CC_MUTE ||
                        cc == CC_RECORD || cc == CC_DELETE ||
                        cc == CC_MIC_IN_DETECT || cc == CC_LINE_OUT_DETECT) {
                        continue; /* Filter UI CCs and LED-only controls */
                    }
                }
            }
        }
        if (notes_only) {
            if ((status & 0xF0) != 0x90 && (status & 0xF0) != 0x80) {
                continue;
            }
        }
        if (ch3_only) {
            if ((status & 0x80) == 0) {
                continue;
            }
            if ((status & 0x0F) != 0x02) {
                continue;
            }
        } else if (block_ch1) {
            if ((status & 0x80) != 0 && (status & 0xF0) < 0xF0 && (status & 0x0F) == 0x00) {
                continue;
            }
        } else if (allow_ch5_8) {
            if ((status & 0x80) == 0) {
                continue;
            }
            if ((status & 0xF0) < 0xF0) {
                uint8_t ch = status & 0x0F;
                if (ch < 0x04 || ch > 0x07) {
                    continue;
                }
            }
        }
        filtered[i] = src[i];
        filtered[i + 1] = src[i + 1];
        filtered[i + 2] = src[i + 2];
        filtered[i + 3] = src[i + 3];
        if (log_on) {
            if (!log) {
                log = fopen("/data/UserData/move-anything/shadow_midi_forward.log", "a");
            }
            if (log) {
                fprintf(log, "fwd: idx=%d cable=%u cin=%u status=%02x d1=%02x d2=%02x\n",
                        i, cable, cin, src[i + 1], src[i + 2], src[i + 3]);
                fflush(log);
            }
        }
        has_midi = 1;
    }

    if (has_midi) {
        memcpy(shadow_midi_shm, filtered, MIDI_BUFFER_SIZE);
        shadow_control->midi_ready++;
    }
}

/* ============================================================================
 * Capture rules lookup
 * ============================================================================ */

/* Get capture rules for the focused slot (0-3 = chain, 4 = master FX) */
const shadow_capture_rules_t *shadow_get_focused_capture(void)
{
    shadow_control_t *shadow_control = *host_shadow_control;
    if (!shadow_control) return NULL;

    int slot = shadow_control->ui_slot;
    if (slot == SHADOW_CHAIN_INSTANCES) {
        /* Master FX is focused (slot 4) */
        return host_master_fx_capture;
    }
    if (slot >= 0 && slot < SHADOW_CHAIN_INSTANCES) {
        return &host_chain_slots[slot].capture;
    }
    return NULL;
}
