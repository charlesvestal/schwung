/* shadow_midi.c - MIDI routing, dispatch, and forwarding
 * Extracted from schwung_shim.c for maintainability. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shadow_midi.h"
#include "shadow_chain_mgmt.h"
#include "shadow_led_queue.h"
#include "shadow_overlay.h"  /* MIDI channel indicator globals */

static void shadow_chain_transpose_reset(void);

/* ============================================================================
 * External cable-2 dispatch ring
 * ============================================================================ */

#define DISPATCHED_EXT_RING_SIZE 32
#define DISPATCHED_EXT_MAX_AGE_TICKS 8

typedef struct {
    uint8_t status;
    uint8_t d1;
    uint8_t d2;
    uint8_t valid;
    uint32_t tick;
} dispatched_ext_entry_t;

static dispatched_ext_entry_t g_dispatched_ext_ring[DISPATCHED_EXT_RING_SIZE];
static int g_dispatched_ext_head = 0;
static uint32_t g_dispatched_ext_tick = 0;

/* Canonicalize note-off forms so the same release event matches whether the
 * keyboard sent 0x8X vel=N or Move's track echoed it as 0x9X vel=0.  Status
 * type is normalized to 0x8X (preserving channel) and release-velocity is
 * normalized to 0.  Channel is PRESERVED so MIDI_OUT echoes that Move's
 * firmware re-emitted on a different channel (track-auto-map) do not falsely
 * match a ring entry recorded from cable-2 on the original channel. */
static void canonicalize_for_ring(uint8_t *status, uint8_t *d1, uint8_t *d2)
{
    (void)d1;
    uint8_t type = *status & 0xF0;
    uint8_t ch   = *status & 0x0F;
    if (type == 0x90 && *d2 == 0) {
        *status = (uint8_t)(0x80 | ch);   /* note-off form, channel kept */
    } else if (type == 0x80) {
        *d2 = 0;                          /* normalize release velocity */
    }
}

void shadow_external_dispatch_tick(void)
{
    g_dispatched_ext_tick++;
}

void shadow_external_dispatch_record(uint8_t status, uint8_t d1, uint8_t d2)
{
    canonicalize_for_ring(&status, &d1, &d2);
    dispatched_ext_entry_t *e = &g_dispatched_ext_ring[g_dispatched_ext_head];
    e->status = status;
    e->d1 = d1;
    e->d2 = d2;
    e->tick = g_dispatched_ext_tick;
    e->valid = 1;
    g_dispatched_ext_head = (g_dispatched_ext_head + 1) % DISPATCHED_EXT_RING_SIZE;
}

int shadow_external_dispatch_was_recent(uint8_t status, uint8_t d1, uint8_t d2)
{
    canonicalize_for_ring(&status, &d1, &d2);
    for (int i = 0; i < DISPATCHED_EXT_RING_SIZE; i++) {
        const dispatched_ext_entry_t *e = &g_dispatched_ext_ring[i];
        if (!e->valid) continue;
        if ((g_dispatched_ext_tick - e->tick) > DISPATCHED_EXT_MAX_AGE_TICKS) continue;
        if (e->status == status && e->d1 == d1 && e->d2 == d2) return 1;
    }
    return 0;
}

/* Per-dispatcher event-id dedup ring.  MIDI_IN events are 8 bytes: a 4-byte
 * USB-MIDI packet plus a 4-byte XMOS-set timestamp that's unique per physical
 * event.  Using the full 8-byte payload as a key, the same physical event
 * dedupes correctly whether it persists at the same offset across frames or
 * shifts down as Move consumes slot 0.  A retriggered note (new XMOS event)
 * has a fresh timestamp, so its key differs and it dispatches normally.
 *
 * Injected packets (chain MIDI FX, davebox ROUTE_MOVE) carry a zero timestamp
 * — they collide if they share USB-MIDI bytes within the staleness window,
 * but they don't reach this dispatcher in normal operation (Move's firmware
 * routes them out of MIDI_IN cable-2 quickly via the cable-0 inject path). */
#define EVENT_DEDUP_RING_SIZE 64
#define EVENT_DEDUP_MAX_AGE_TICKS 16

typedef struct {
    uint8_t key[8];
    uint32_t tick;
    uint8_t valid;
} event_dedup_entry_t;

static event_dedup_entry_t g_thru_dedup[EVENT_DEDUP_RING_SIZE];
static int g_thru_dedup_head = 0;
static event_dedup_entry_t g_ch_dedup[EVENT_DEDUP_RING_SIZE];
static int g_ch_dedup_head = 0;

/* Returns 1 if this 8-byte event was recently dispatched and should be
 * skipped, 0 otherwise.  Records the event in the ring when it passes.
 *
 * Events with a zero timestamp bypass the dedup ring entirely (return 0,
 * no record).  shadow_drain_midi_inject memsets the timestamp field, and
 * fast-repeating same-pitch injects (e.g. an arpeggiator pulsing a held
 * note) would otherwise key-collide with each other and be silently
 * swallowed.  This means cable-2 re-injects from shim_forward_cable2_to_move
 * are not deduped via this path — that's handled separately by injecting
 * those re-injects as cable-0 so the cable-2 dispatchers don't see them.
 *
 * Events with a non-zero timestamp (XMOS-written external MIDI) go through
 * the ring: same physical event keeps its timestamp across frames whether
 * it persists at the same offset or shifts as Move clears slot 0, so the
 * dispatcher fires exactly once per physical event. */
static int event_dedup_check_and_record(event_dedup_entry_t *ring, int *head,
                                        const uint8_t *key)
{
    int has_timestamp = key[4] || key[5] || key[6] || key[7];
    if (!has_timestamp) return 0;
    for (int i = 0; i < EVENT_DEDUP_RING_SIZE; i++) {
        if (!ring[i].valid) continue;
        if ((g_dispatched_ext_tick - ring[i].tick) > EVENT_DEDUP_MAX_AGE_TICKS) continue;
        if (memcmp(ring[i].key, key, 8) == 0) return 1;
    }
    event_dedup_entry_t *e = &ring[*head];
    memcpy(e->key, key, 8);
    e->tick = g_dispatched_ext_tick;
    e->valid = 1;
    *head = (*head + 1) % EVENT_DEDUP_RING_SIZE;
    return 0;
}

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
static shadow_midi_inject_t **host_shadow_midi_inject_shm;
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
    host_shadow_midi_inject_shm = host->shadow_midi_inject_shm;
    host_shadow_mailbox = host->shadow_mailbox;
    host_master_fx_capture = host->master_fx_capture;
    host_slot_idle = host->slot_idle;
    host_slot_silence_frames = host->slot_silence_frames;
    host_slot_fx_idle = host->slot_fx_idle;
    host_slot_fx_silence_frames = host->slot_fx_silence_frames;

    shadow_chain_transpose_reset();
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

/* Per-slot active-note tracker: remembers the *transposed* note value that
 * was actually dispatched on note-on, keyed by (slot, channel, original note).
 * 0xFF means the note is not currently held.
 *
 * This lets a note-off close the same note that its note-on opened, even if
 * the slot's transpose amount changed while the note was held — otherwise
 * changing transpose during a sustained note leaves stuck notes because the
 * note-off arrives with a different transposed value than the note-on used. */
static uint8_t slot_active_note[SHADOW_CHAIN_INSTANCES][16][128];

static void shadow_chain_transpose_reset(void)
{
    memset(slot_active_note, 0xFF, sizeof(slot_active_note));
}

/* Apply per-slot semitone transpose to a 3-byte MIDI message in place.
 * Only affects note-off (0x80), note-on (0x90), and poly aftertouch (0xA0) —
 * all other channel-voice messages pass through unchanged.
 * Returns 1 if the message should be dispatched, 0 if a note-on would fall
 * outside 0-127 (and must be dropped without registering as held). */
static int shadow_chain_apply_transpose(int slot, uint8_t *msg)
{
    uint8_t type = msg[0] & 0xF0;
    if (type != 0x80 && type != 0x90 && type != 0xA0) return 1;

    uint8_t ch = msg[0] & 0x0F;
    uint8_t orig = msg[1];
    int transpose = host_chain_slots[slot].transpose;

    /* Note-on with velocity > 0: apply current transpose and remember it. */
    if (type == 0x90 && msg[2] > 0) {
        int note = (int)orig + transpose;
        if (note < 0 || note > 127) return 0;
        slot_active_note[slot][ch][orig] = (uint8_t)note;
        msg[1] = (uint8_t)note;
        return 1;
    }

    /* Note-off (or note-on with vel 0): reuse the transposed value from
     * note-on so the synth closes the correct voice regardless of current
     * transpose. Fall back to current transpose if we have no record. */
    if (type == 0x80 || type == 0x90) {
        uint8_t held = slot_active_note[slot][ch][orig];
        if (held != 0xFF) {
            msg[1] = held;
            slot_active_note[slot][ch][orig] = 0xFF;
            return 1;
        }
        if (transpose == 0) return 1;
        int note = (int)orig + transpose;
        if (note < 0 || note > 127) return 0;
        msg[1] = (uint8_t)note;
        return 1;
    }

    /* Poly aftertouch: match the held note's transposed value if we know it. */
    uint8_t held = slot_active_note[slot][ch][orig];
    if (held != 0xFF) {
        msg[1] = held;
        return 1;
    }
    if (transpose == 0) return 1;
    int note = (int)orig + transpose;
    if (note < 0 || note > 127) return 0;
    msg[1] = (uint8_t)note;
    return 1;
}

/* ============================================================================
 * MIDI dispatch to chain slots
 * ============================================================================ */

/* Dispatch MIDI to all matching slots (supports recv=All broadcasting).
 * When skip_direct is 1, slots with receive=All and forward=THRU are skipped
 * because they receive MIDI via the direct MIDI_IN path instead. */
void shadow_chain_dispatch_midi_to_slots(const uint8_t *pkt, int log_on, int *midi_log_count, int skip_direct)
{
    const plugin_api_v2_t *pv2 = *host_plugin_v2;
    uint8_t status_usb = pkt[1];
    uint8_t type = status_usb & 0xF0;
    uint8_t midi_ch = status_usb & 0x0F;
    uint8_t note = pkt[2];
    int dispatched = 0;

    /* NOTE: the MIDI channel indicator is no longer driven from here. This
     * dispatch path fires for every source (cable-2 inbound, MIDI_OUT echo,
     * overtake DSP, UI-drain) so the channel it saw reflected post-routing /
     * echo, not what the external controller actually sent. The indicator is
     * now updated from a dedicated cable-2 MIDI_IN scan in the shim
     * (shim_pre_transfer) so it reports the incoming external channel. */

    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        /* Skip direct-dispatch slots when processing MIDI_OUT.
         * These slots get MIDI from MIDI_IN directly to preserve
         * original channels for MPE. */
        if (skip_direct &&
            host_chain_slots[i].channel == -1 &&
            host_chain_slots[i].forward_channel == -2)
            continue;

        /* Check channel match: slot receives this channel, or slot is set to All (-1) */
        int channel_matched = (host_chain_slots[i].channel == (int)midi_ch || host_chain_slots[i].channel == -1);
        
        // If split is enabled for this slot's parent, the split slot matches because its parent matches!
        if (!channel_matched && i >= 4 && host_chain_slots[i - 4].split_enabled) {
            int parent_ch = host_chain_slots[i - 4].channel;
            if (parent_ch == (int)midi_ch || parent_ch == -1) {
                channel_matched = 1;
            }
        }
        
        if (!channel_matched)
            continue;

        // Apply keyboard splitting if enabled on the parent slot
        int parent_slot = (i < 4) ? i : (i - 4);
        int split_slot = parent_slot + 4;
        if (host_chain_slots[parent_slot].split_enabled) {
            int split_point = host_chain_slots[parent_slot].split_octave * 12;
            
            if (type == 0x90 && pkt[3] > 0) { // Note On
                if (note < split_point) {
                    // Below boundary: Route ONLY to parent slot (skip if current slot is split)
                    if (i == split_slot) continue;
                } else {
                    // At or above boundary: Route ONLY to split slot (skip if current slot is parent)
                    if (i == parent_slot) continue;
                }
            } else if (type == 0x80 || (type == 0x90 && pkt[3] == 0) || type == 0xA0) { // Note Off / Poly AT
                // Route to whichever slot actually has the note marked active
                uint8_t held_parent = slot_active_note[parent_slot][midi_ch][note];
                uint8_t held_split = slot_active_note[split_slot][midi_ch][note];
                
                if (held_split != 0xFF) {
                    // Active on split slot, skip parent slot
                    if (i == parent_slot) continue;
                } else if (held_parent != 0xFF) {
                    // Active on parent slot, skip split slot
                    if (i == split_slot) continue;
                } else {
                    // Fallback if untracked (use split boundary)
                    if (note < split_point) {
                        if (i == split_slot) continue;
                    } else {
                        if (i == parent_slot) continue;
                    }
                }
            }
        }

        /* Lazy activation check — any loaded component (synth, audio FX,
         * or MIDI FX) is enough to activate. MIDI-FX-only slots in Pre
         * mode have no synth or audio FX but still need to dispatch
         * incoming MIDI to drive the FX and inject to Move. */
        if (!host_chain_slots[i].active) {
            if (pv2 && pv2->get_param &&
                host_chain_slots[i].instance) {
                static const char *probe_keys[] = {
                    "synth_module", "fx1_module", "fx2_module",
                    "midi_fx1_module", "midi_fx2_module"
                };
                for (size_t k = 0; k < sizeof(probe_keys)/sizeof(probe_keys[0]); k++) {
                    char buf[64];
                    int len = pv2->get_param(host_chain_slots[i].instance,
                                              probe_keys[k], buf, sizeof(buf));
                    if (len <= 0) continue;
                    if (len < (int)sizeof(buf)) buf[len] = '\0';
                    else buf[sizeof(buf) - 1] = '\0';
                    if (buf[0] != '\0') {
                        host_chain_slots[i].active = 1;
                        if (host_ui_state_update_slot)
                            host_ui_state_update_slot(i);
                        break;
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
            if (shadow_chain_apply_transpose(i, msg)) {
                pv2->on_midi(host_chain_slots[i].instance, msg, 3,
                             MOVE_MIDI_SOURCE_EXTERNAL);
            }
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
 * so the DSP routing can pick them up alongside the echoed notes.
 *
 * Note: Move may remap note channels via its track auto-mapping, but CCs here
 * preserve the original controller channel. For CC routing to work, the external
 * controller, Move track, and shadow slot receive channel must all be set to the
 * same explicit channel (don't rely on Move's auto channel mapping). */
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
        /* MIDI_OUT region is 80 bytes; display starts at 80. Don't write past. */
        while (hw_offset < HW_MIDI_OUT_SIZE) {
            if (midi_out[hw_offset] == 0 && midi_out[hw_offset+1] == 0 &&
                midi_out[hw_offset+2] == 0 && midi_out[hw_offset+3] == 0) {
                break;
            }
            hw_offset += 4;
        }
        if (hw_offset >= HW_MIDI_OUT_SIZE) break;  /* Buffer full */

        memcpy(&midi_out[hw_offset], &local_buf[i], 4);
        hw_offset += 4;
    }
}

/* Drain MIDI inject buffer into Move's MIDI_IN (post-ioctl).
 * Copies USB-MIDI packets from SHM into empty slots in shadow_mailbox+MIDI_IN_OFFSET,
 * making Move process them as if they came from physical hardware.
 * Rate-limited to 16 packets per tick to avoid flooding. */
void shadow_drain_midi_inject(void)
{
    shadow_midi_inject_t *inject_shm = *host_shadow_midi_inject_shm;
    static uint8_t last_ready = 0;
    /* MIDI_IN events are 8 bytes each (4 USB-MIDI + 4 timestamp). Scanning
     * at 4-byte stride would land mid-event and corrupt Move's parse (Move
     * terminates the scan at the first zero slot, so any gap loses all
     * following real events). Use 8-byte stride + byte-0 check for empty. */
    const int MIDI_IN_EVT_STRIDE = 8;
    const int MIDI_IN_MAX_EVTS   = 31;              /* SCHWUNG_MIDI_IN_MAX */
    const int MIDI_IN_MAX_BYTES  = MIDI_IN_EVT_STRIDE * MIDI_IN_MAX_EVTS;

    if (!inject_shm) return;

    /* Defer inject when MIDI_IN has ANY hardware events this tick.
     * All cables share Move's firmware MIDI read path — injecting concurrently
     * with hardware events on ANY cable races Move's internal processing and
     * causes SIGABRT. The original guard only checked cable-0 (pads), but
     * empirically the crash occurs with inject at offset 24 when events on
     * other cables occupy offsets 0/8/16 — confirming the guard must be
     * cable-agnostic.  Move also partially consumes MIDI_IN (clearing slot 0
     * while leaving events at higher offsets), so checking only slot 0 misses
     * residual events; scan all slots.
     *
     * Rule: if any MIDI_IN slot is non-zero this tick, reset the defer counter.
     * Otherwise, drain once the counter has climbed to 2 (≈6ms at 2.9ms/tick).
     * Pattern from chord-mode-native (shadow_chord.c CHORD_DEFER_FRAMES). */
    const int DEFER_FRAMES = 2;
    static int defer_counter = 0;
    uint8_t *midi_in_scan = host_shadow_mailbox + MIDI_IN_OFFSET;
    int hw_cable_active = 0;
    for (int j = 0; j < MIDI_IN_MAX_BYTES; j += MIDI_IN_EVT_STRIDE)
        if (midi_in_scan[j] != 0) { hw_cable_active = 1; break; }
    if (hw_cable_active) {
        defer_counter = 0;
        return;
    }
    if (defer_counter < DEFER_FRAMES) {
        defer_counter++;
        return;
    }

    if (inject_shm->ready == last_ready) return;

    last_ready = inject_shm->ready;

    /* Snapshot buffer first, then reset write_idx */
    int snapshot_len = inject_shm->write_idx;
    uint8_t local_buf[SHADOW_MIDI_INJECT_BUFFER_SIZE];
    int copy_len = snapshot_len < (int)SHADOW_MIDI_INJECT_BUFFER_SIZE
                 ? snapshot_len : (int)SHADOW_MIDI_INJECT_BUFFER_SIZE;
    if (copy_len > 0) {
        memcpy(local_buf, inject_shm->buffer, copy_len);
    }
    __sync_synchronize();
    inject_shm->write_idx = 0;
    memset(inject_shm->buffer, 0, SHADOW_MIDI_INJECT_BUFFER_SIZE);

    if (copy_len <= 0) return;

    /* Inject into shadow_mailbox at MIDI_IN_OFFSET */
    uint8_t *midi_in = host_shadow_mailbox + MIDI_IN_OFFSET;

    int hw_offset = 0;
    int injected = 0;
    int consumed_bytes = 0;
    for (int i = 0; i < copy_len && injected < 16; i += 4) {
        /* Find empty 8-byte slot (byte 0 == 0 means no cable/CIN, unused) */
        int saw_existing = 0;
        while (hw_offset < MIDI_IN_MAX_BYTES) {
            if (midi_in[hw_offset] == 0) break;
            saw_existing = 1;
            hw_offset += MIDI_IN_EVT_STRIDE;
        }
        if (hw_offset >= MIDI_IN_MAX_BYTES) break;  /* Buffer full */
        /* If we skipped over pre-existing events to find an empty slot, bail
         * and carry remaining packets to the next frame.  The defer guard has
         * a narrow race window: events can appear in MIDI_IN between the guard
         * check and the write.  Injecting alongside hardware events (at a
         * non-zero offset) races Move's firmware MIDI read path and causes
         * SIGABRT — empirically the crash always fires at a non-zero offset. */
        if (saw_existing) break;

        /* Write 4-byte USB-MIDI packet + zero the 4-byte timestamp.
         * Cable nibble in local_buf[i] is preserved — callers choose cable:
         * 0 for internal hardware (pads/buttons, Move-prefix protocol),
         * 2 for external USB (general MIDI, routed to tracks by channel). */
        memcpy(&midi_in[hw_offset], &local_buf[i], 4);
        memset(&midi_in[hw_offset + 4], 0, 4);
        hw_offset += MIDI_IN_EVT_STRIDE;
        injected++;
        consumed_bytes = i + 4;
    }

    /* Carry over any undrained packets so they fire in subsequent frames.
     * Without this, bursts that exceed the 8-per-frame cap (or pile up
     * during DEFER_FRAMES while cable-0 is active) lose events when the
     * snapshot resets write_idx above — which strands note-offs and leaves
     * voices hanging on Move's track. Safe to rewrite inject_shm->buffer
     * here because shadow_chain_midi_inject runs on the same SPI thread. */
    int remaining = copy_len - consumed_bytes;
    if (remaining > 0) {
        /* write_idx is uint8_t (0..255) and the inject guard checks wr+4 <= 256,
         * so write_idx must never exceed 252 (63 packets). Cap carryover at
         * 252 so a new inject from chain_host can still land. */
        if (remaining > 252) remaining = 252;
        memcpy(inject_shm->buffer, &local_buf[consumed_bytes], remaining);
        inject_shm->write_idx = (uint8_t)remaining;
        inject_shm->ready++;  /* re-arm so next frame drains without waiting
                               * for a new inject to bump ready */
    }

    if (host_log && injected > 0) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "MIDI inject: drained %d/%d pkts at offset %d%s",
                 injected, copy_len / 4,
                 hw_offset - injected * MIDI_IN_EVT_STRIDE,
                 remaining > 0 ? " [carryover]" : "");
        host_log(dbg);
    }
}

/* Queue a 4-byte USB-MIDI packet for MIDI_IN injection.
 * Called by chain MIDI FX in Pre mode so Move's native instrument receives
 * the transformed stream alongside the slot synth.
 *
 * The cable nibble in msg[0] is preserved by the drain. For Pre-mode chain
 * use, set cable = 2 (external USB) so Move routes by channel to the track
 * instrument; cable 0 is reserved for Move's pad/button prefix protocol and
 * won't reach track instruments for pitched notes.
 *
 * The drain rate-limits to 8 packets/tick; callers should not burst more
 * than that per render block. Same-thread as the drain (both run in the
 * shim's SPI loop), so no extra synchronization is needed beyond the
 * existing ring pattern. */
int shadow_chain_midi_inject(const uint8_t *msg, int len)
{
    if (!msg || len != 4) return 0;
    if (!host_shadow_midi_inject_shm) return 0;
    shadow_midi_inject_t *shm = *host_shadow_midi_inject_shm;
    if (!shm) return 0;

    int wr = shm->write_idx;
    if (wr + 4 > (int)SHADOW_MIDI_INJECT_BUFFER_SIZE) {
        if (host_log) {
            char dbg[96];
            uint8_t type = msg[1] & 0xF0;
            snprintf(dbg, sizeof(dbg),
                     "MIDI inject FULL: wr=%d dropped status=0x%02x n=%d v=%d",
                     wr, msg[1], msg[2], msg[3]);
            host_log(dbg);
        }
        return 0;
    }

    memcpy(&shm->buffer[wr], msg, 4);
    shm->write_idx = (uint8_t)(wr + 4);
    __sync_synchronize();
    shm->ready++;
    return 4;
}

/* Drain MIDI-to-DSP buffer from shadow UI and dispatch to chain slots. */
void shadow_drain_ui_midi_dsp(void)
{
    shadow_midi_dsp_t *midi_dsp_shm = *host_shadow_midi_dsp_shm;
    static uint8_t last_ready = 0;

    if (!midi_dsp_shm) return;
    if (midi_dsp_shm->ready == last_ready) return;

    last_ready = midi_dsp_shm->ready;

    /* Snapshot buffer before resetting to avoid race with JS writer.
     * Barrier ensures we see the buffer data that corresponds to the ready signal. */
    __sync_synchronize();
    int snapshot_len = midi_dsp_shm->write_idx;
    uint8_t local_buf[SHADOW_MIDI_DSP_BUFFER_SIZE];
    int copy_len = snapshot_len < (int)SHADOW_MIDI_DSP_BUFFER_SIZE
                 ? snapshot_len : (int)SHADOW_MIDI_DSP_BUFFER_SIZE;
    if (copy_len > 0) {
        memcpy(local_buf, midi_dsp_shm->buffer, copy_len);
    }
    __sync_synchronize();
    midi_dsp_shm->write_idx = 0;
    memset(midi_dsp_shm->buffer, 0, SHADOW_MIDI_DSP_BUFFER_SIZE);

    static int midi_log_count = 0;
    int log_on = host_midi_out_log_enabled ? host_midi_out_log_enabled() : 0;

    for (int i = 0; i < copy_len; i += 4) {
        uint8_t status = local_buf[i];
        uint8_t d1 = local_buf[i + 1];
        uint8_t d2 = local_buf[i + 2];

        /* Validate status byte has high bit set */
        if (!(status & 0x80)) continue;

        /* Construct USB-MIDI packet for dispatch: [CIN, status, d1, d2] */
        uint8_t cin = (status >> 4) & 0x0F;
        uint8_t pkt[4] = { cin, status, d1, d2 };

        shadow_chain_dispatch_midi_to_slots(pkt, log_on, &midi_log_count, 0);
    }
}

/* ============================================================================
 * Direct external MIDI dispatch (MPE passthrough)
 * ============================================================================ */

/* Dispatch external MIDI from MIDI_IN cable 2 directly to slots configured
 * for passthrough (receive=All, forward=THRU).  This bypasses Move's MIDI_OUT
 * so that notes and per-note expression data (pitch bend, CC, aftertouch)
 * arrive on their original channels — required for MPE controllers. */
void shadow_dispatch_direct_external_midi(void)
{
    if (!*host_shadow_inprocess_ready || !*host_global_mmap_addr) return;

    const plugin_api_v2_t *pv2 = *host_plugin_v2;
    if (!pv2 || !pv2->on_midi) return;

    /* Check whether any slot qualifies for direct dispatch. */
    int has_direct = 0;
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++) {
        if (host_chain_slots[i].channel == -1 &&
            host_chain_slots[i].forward_channel == -2) {
            has_direct = 1;
            break;
        }
    }
    if (!has_direct) return;

    uint8_t *in_src = *host_global_mmap_addr + MIDI_IN_OFFSET;

    /* Walk MIDI_IN at the protocol-correct 8-byte stride (4 bytes USB-MIDI +
     * 4 bytes XMOS timestamp).  Don't break on zero — Move's firmware
     * partially consumes MIDI_IN and events at higher offsets may shift
     * down or persist across frames; we rely on the timestamp-keyed dedup
     * to skip duplicates regardless of where the event lives. */
    for (int i = 0; i + 8 <= MIDI_BUFFER_SIZE; i += 8) {
        uint8_t cin = in_src[i] & 0x0F;
        uint8_t cable = (in_src[i] >> 4) & 0x0F;

        /* Only external USB MIDI (cable 2) */
        if (cable != 0x02) continue;
        if (cin < 0x08 || cin > 0x0E) continue;

        uint8_t status = in_src[i + 1];
        uint8_t type = status & 0xF0;
        uint8_t d1 = in_src[i + 2];
        uint8_t d2 = in_src[i + 3];

        /* Valid channel voice messages only (0x80-0xE0) */
        if (type < 0x80 || type > 0xE0) continue;

        /* CIN must match status type */
        if (cin != (type >> 4)) continue;

        /* Data bytes must be 0-127 */
        if ((d1 & 0x80) || (d2 & 0x80)) continue;

        /* Filter knob touch notes (0-9) from internal MIDI */
        if ((type == 0x90 || type == 0x80) && d1 < 10) continue;

        /* Skip if we already dispatched this physical event (same content +
         * timestamp) within the recent window. */
        if (event_dedup_check_and_record(g_thru_dedup, &g_thru_dedup_head, &in_src[i]))
            continue;

        /* Record valid cable-2 voice events so the MIDI_OUT echo check can
         * identify them even after the MIDI_IN slot has been reused. */
        shadow_external_dispatch_record(status, d1, d2);

        /* Dispatch to qualifying slots: receive=All, forward=THRU */
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (host_chain_slots[s].channel != -1 ||
                host_chain_slots[s].forward_channel != -2)
                continue;

            /* Lazy activation (same logic as main dispatch) */
            if (!host_chain_slots[s].active) {
                if (host_chain_slots[s].instance) {
                    char buf[64];
                    int len = pv2->get_param(host_chain_slots[s].instance,
                                              "synth_module", buf, sizeof(buf));
                    if (len > 0) {
                        if (len < (int)sizeof(buf)) buf[len] = '\0';
                        else buf[sizeof(buf) - 1] = '\0';
                        if (buf[0] != '\0') {
                            host_chain_slots[s].active = 1;
                            if (host_ui_state_update_slot)
                                host_ui_state_update_slot(s);
                        }
                    }
                }
                if (!host_chain_slots[s].active) continue;
            }

            /* Wake from idle */
            if (host_slot_idle[s] || host_slot_fx_idle[s]) {
                host_slot_idle[s] = 0;
                host_slot_silence_frames[s] = 0;
                host_slot_fx_idle[s] = 0;
                host_slot_fx_silence_frames[s] = 0;
            }

            /* Send with original channel preserved (THRU mode) */
            uint8_t msg[3] = { status, d1, d2 };
            if (shadow_chain_apply_transpose(s, msg)) {
                pv2->on_midi(host_chain_slots[s].instance, msg, 3,
                             MOVE_MIDI_SOURCE_EXTERNAL);
            }
        }

        /* Broadcast to audio FX on all active slots */
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!host_chain_slots[s].active || !host_chain_slots[s].instance)
                continue;
            uint8_t msg[3] = { status, d1, d2 };
            pv2->on_midi(host_chain_slots[s].instance, msg, 3,
                         MOVE_MIDI_SOURCE_FX_BROADCAST);
        }

        /* Forward to master FX */
        {
            uint8_t msg[3] = { status, d1, d2 };
            if (host_master_fx_forward_midi)
                host_master_fx_forward_midi(msg, 3, MOVE_MIDI_SOURCE_EXTERNAL);
        }
    }
}

/* Dispatch cable-2 (USB-A external MIDI) note/voice messages to chain slots
 * matched by configured receive channel.  Called when no tool module is
 * active so that Schwung chain instruments respond to external MIDI by channel
 * just as they would via Move's normal MIDI routing when Schwung is not
 * installed. */
void shadow_dispatch_cable2_channeled_slots(void)
{
    if (!*host_shadow_inprocess_ready || !*host_global_mmap_addr) return;

    const plugin_api_v2_t *pv2 = *host_plugin_v2;
    if (!pv2 || !pv2->on_midi) return;

    uint8_t *in_src = *host_global_mmap_addr + MIDI_IN_OFFSET;

    /* 8-byte stride (USB-MIDI + timestamp); don't break on zero. */
    for (int i = 0; i + 8 <= MIDI_BUFFER_SIZE; i += 8) {
        uint8_t header = in_src[i];
        if (header == 0) continue;

        uint8_t cable = (header >> 4) & 0x0F;
        if (cable != 0x02) continue;   /* only cable-2 (USB-A external MIDI) */

        uint8_t cin    = header & 0x0F;
        uint8_t status = in_src[i + 1];
        uint8_t type   = status & 0xF0;
        uint8_t d1     = in_src[i + 2];
        uint8_t d2     = in_src[i + 3];

        /* Channel voice messages only */
        if (cin < 0x08 || cin > 0x0E) continue;
        if (type < 0x80 || type > 0xE0) continue;
        if (cin != (type >> 4)) continue;

        /* Data bytes must be 0-127 */
        if ((d1 & 0x80) || (d2 & 0x80)) continue;

        /* Filter knob-touch notes (internal Move notes 0-9) */
        if ((type == 0x90 || type == 0x80) && d1 < 10) continue;

        /* Skip if this physical event (content + timestamp) was already
         * dispatched recently. */
        if (event_dedup_check_and_record(g_ch_dedup, &g_ch_dedup_head, &in_src[i]))
            continue;

        /* Record valid cable-2 voice events so the MIDI_OUT echo check can
         * identify them even after the MIDI_IN slot has been reused. */
        shadow_external_dispatch_record(status, d1, d2);

        uint8_t in_ch = status & 0x0F;

        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            int slot_ch = host_chain_slots[s].channel;

            /* Skip THRU slots — already handled by shadow_dispatch_direct_external_midi */
            if (slot_ch == -1 && host_chain_slots[s].forward_channel == -2) continue;

            /* Channel filter: -1 = receive all; >= 0 = specific channel */
            if (slot_ch >= 0 && slot_ch != (int)in_ch) continue;

            /* Lazy activation */
            if (!host_chain_slots[s].active) {
                if (host_chain_slots[s].instance) {
                    char buf[64];
                    int len = pv2->get_param(host_chain_slots[s].instance,
                                              "synth_module", buf, sizeof(buf));
                    if (len > 0) {
                        if (len < (int)sizeof(buf)) buf[len] = '\0';
                        else buf[sizeof(buf) - 1] = '\0';
                        if (buf[0] != '\0') {
                            host_chain_slots[s].active = 1;
                            if (host_ui_state_update_slot)
                                host_ui_state_update_slot(s);
                        }
                    }
                }
                if (!host_chain_slots[s].active) continue;
            }

            /* Wake from idle */
            if (host_slot_idle[s] || host_slot_fx_idle[s]) {
                host_slot_idle[s]          = 0;
                host_slot_silence_frames[s] = 0;
                host_slot_fx_idle[s]        = 0;
                host_slot_fx_silence_frames[s] = 0;
            }

            uint8_t msg[3] = { status, d1, d2 };
            if (shadow_chain_apply_transpose(s, msg))
                pv2->on_midi(host_chain_slots[s].instance, msg, 3,
                             MOVE_MIDI_SOURCE_EXTERNAL);
        }
    }
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
        cached_ch3_only = (access("/data/UserData/schwung/shadow_midi_ch3_only", F_OK) == 0);
        cached_block_ch1 = (access("/data/UserData/schwung/shadow_midi_block_ch1", F_OK) == 0);
        cached_allow_ch5_8 = (access("/data/UserData/schwung/shadow_midi_allow_ch5_8", F_OK) == 0);
        cached_notes_only = (access("/data/UserData/schwung/shadow_midi_notes_only", F_OK) == 0);
        cached_allow_cable0 = (access("/data/UserData/schwung/shadow_midi_allow_cable0", F_OK) == 0);
        cached_drop_cable_f = (access("/data/UserData/schwung/shadow_midi_drop_cable_f", F_OK) == 0);
        cached_log_on = (access("/data/UserData/schwung/shadow_midi_log_on", F_OK) == 0);
        cached_drop_ui = (access("/data/UserData/schwung/shadow_midi_drop_ui", F_OK) == 0);
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
                log = fopen("/data/UserData/schwung/shadow_midi_forward.log", "a");
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
