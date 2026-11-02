/* Regression test: external MIDI must reach audio FX and Master FX whether or
 * not a THRU slot exists.
 *
 * Audio FX that listen to MIDI (a ducker keyed to a kick, say) are fed by
 * FX_BROADCAST to every active slot and by the Master FX forward. Cable-2
 * (USB-A) input reaches the slots by one of two paths each frame:
 * shadow_dispatch_direct_external_midi when a THRU slot exists, which
 * broadcasts every event, and shadow_dispatch_cable2_channeled_slots
 * otherwise, which delivered to channel-matched slots only. The MIDI_OUT echo
 * of an external event is skipped outside overtake (is_external_echo in the
 * shim), so it fed no audio FX either. With no THRU slot, then, a note on a
 * channel no slot receives reached no audio FX at all: a ducker listening on
 * its own channel to external gear stayed silent, and adding an unrelated
 * THRU slot made it work.
 *
 * This drives the real shadow_midi.c through midi_routing_init with fake
 * slots, a fake MIDI_IN buffer and a recording on_midi, and counts who
 * receives each event.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shadow_midi.h"
#include "shadow_midi_filter.h"
#include "shadow_overtake_midi.h"
#include "shadow_chain_mgmt.h"
#include "lane_trace.h"
#include "shim_worker.h"

/* --- Link stubs: shadow_midi.c references these from paths not under test -- */
int capture_has_cc(const shadow_capture_rules_t *rules, uint8_t cc) { (void)rules; (void)cc; return 0; }
int capture_has_note(const shadow_capture_rules_t *rules, uint8_t note) { (void)rules; (void)note; return 0; }
lane_trace_t *lane_trace_ring(void) { return NULL; }
int lane_trace_armed(void) { return 0; }
int shadow_master_fx_captures_cc(uint8_t cc) { (void)cc; return 0; }
int shadow_master_fx_captures_note(uint8_t note) { (void)note; return 0; }
void shadow_overtake_midi_init(void) {}
int shadow_overtake_midi_drain(shadow_midi_inject_t *shared, shadow_midi_inject_t *ui,
                               int overtake_active, uint8_t *midi_in,
                               int max_events, int midi_in_bytes)
{
    (void)shared; (void)ui; (void)overtake_active; (void)midi_in;
    (void)max_events; (void)midi_in_bytes;
    return 0;
}
int shadow_slot_has_loaded_component(const plugin_api_v2_t *pv2, void *instance) { (void)pv2; (void)instance; return 1; }
volatile int shim_ui_midi_out_drops, shim_ui_midi_out_placed, shim_ui_midi_out_stranded,
             shim_ui_midi_out_foreign, shim_ui_midi_out_retries, shim_ui_midi_out_unretryable;

/* --- Recording fakes -------------------------------------------------------- */
typedef struct { int slot; uint8_t msg[3]; int source; } call_t;
static call_t slot_calls[64];
static int n_slot_calls;
static call_t mfx_calls[64];
static int n_mfx_calls;

static void fake_on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    if (len < 3 || n_slot_calls >= 64) return;
    call_t *c = &slot_calls[n_slot_calls++];
    c->slot = (int)(intptr_t)instance - 1;   /* instance = slot index + 1 */
    memcpy(c->msg, msg, 3);
    c->source = source;
}

static int fake_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    (void)instance; (void)key; (void)buf; (void)buf_len;
    return 0;
}

static void fake_master_fx(const uint8_t *msg, int len, int source)
{
    if (len < 3 || n_mfx_calls >= 64) return;
    call_t *c = &mfx_calls[n_mfx_calls++];
    c->slot = -1;
    memcpy(c->msg, msg, 3);
    c->source = source;
}

/* --- Host state handed to midi_routing_init -------------------------------- */
static shadow_chain_slot_t slots[SHADOW_CHAIN_INSTANCES];
static plugin_api_v2_t fake_api;
static const plugin_api_v2_t *volatile api_ptr = &fake_api;
static shadow_control_t *volatile ctl_ptr = NULL;
static unsigned char mmap_buf[MIDI_IN_OFFSET + SHADOW_MIDI_IN_BYTES + 64];
static unsigned char *mmap_ptr = mmap_buf;
static int inprocess_ready = 1;
static uint8_t display_mode;
static int slot_idle[SHADOW_CHAIN_INSTANCES], slot_silence[SHADOW_CHAIN_INSTANCES];
static int fx_idle[SHADOW_CHAIN_INSTANCES], fx_silence[SHADOW_CHAIN_INSTANCES];

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static void reset(void)
{
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < SHADOW_CHAIN_INSTANCES; i++)
        slots[i].forward_channel = SHADOW_FORWARD_AUTO;
    memset(mmap_buf, 0, sizeof(mmap_buf));
    n_slot_calls = 0;
    n_mfx_calls = 0;
}

/* A slot that is loaded and running, receiving one channel (0-15) or all (-1). */
static void load_slot(int s, int channel, int forward_channel)
{
    slots[s].instance = (void *)(intptr_t)(s + 1);
    slots[s].active = 1;
    slots[s].channel = channel;
    slots[s].forward_channel = forward_channel;
}

/* One cable-2 channel-voice event in MIDI_IN slot k. The timestamp makes each
 * physical event distinct to the dedup ring, which keys on content + time. */
static void put_event(int k, uint8_t status, uint8_t d1, uint8_t d2, uint32_t ts)
{
    unsigned char *p = mmap_buf + MIDI_IN_OFFSET + 8 * k;
    p[0] = (uint8_t)(0x20 | (status >> 4));   /* cable 2, CIN = message type */
    p[1] = status;
    p[2] = d1;
    p[3] = d2;
    memcpy(p + 4, &ts, 4);
}

static int count(const call_t *calls, int n, int slot, int source)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if ((slot < 0 || calls[i].slot == slot) && calls[i].source == source) c++;
    return c;
}

static void test_unmatched_channel_reaches_fx(void)
{
    reset();
    load_slot(0, 0, SHADOW_FORWARD_AUTO);          /* receives channel 1 */
    put_event(0, 0x9E, 36, 100, 1001);             /* note-on, channel 15 */
    shadow_dispatch_cable2_channeled_slots();

    CHECK(count(slot_calls, n_slot_calls, 0, MOVE_MIDI_SOURCE_EXTERNAL) == 0,
          "a note on a channel the slot does not receive must not reach its synth");
    CHECK(count(slot_calls, n_slot_calls, 0, MOVE_MIDI_SOURCE_FX_BROADCAST) == 1,
          "a note on a channel no slot receives must still reach the slot's audio FX");
    CHECK(n_mfx_calls == 1, "a note on a channel no slot receives must still reach Master FX");
    if (n_mfx_calls == 1)
        CHECK(mfx_calls[0].msg[0] == 0x9E && mfx_calls[0].msg[1] == 36 && mfx_calls[0].msg[2] == 100,
              "Master FX must see the event as played");
}

static void test_matched_channel_also_broadcast(void)
{
    /* Same as the other two dispatch paths: a slot that receives the event
     * normally also gets the broadcast, since FX_BROADCAST is how audio FX in
     * every slot hear the event. */
    reset();
    load_slot(0, 0, SHADOW_FORWARD_AUTO);
    put_event(0, 0x90, 60, 90, 2001);              /* note-on, channel 1 */
    shadow_dispatch_cable2_channeled_slots();

    CHECK(count(slot_calls, n_slot_calls, 0, MOVE_MIDI_SOURCE_EXTERNAL) == 1,
          "a note on the slot's channel must reach its synth once");
    CHECK(count(slot_calls, n_slot_calls, 0, MOVE_MIDI_SOURCE_FX_BROADCAST) == 1,
          "a note on the slot's channel must also be broadcast to audio FX");
    CHECK(n_mfx_calls == 1, "a note on the slot's channel must reach Master FX once");
}

static void test_broadcast_reaches_every_active_slot(void)
{
    /* The behaviour this change gives external MIDI, matching the THRU path
     * and the MIDI_OUT echo: every active slot's audio FX hear every note. */
    reset();
    load_slot(0, 0, SHADOW_FORWARD_AUTO);
    load_slot(2, 5, SHADOW_FORWARD_AUTO);
    put_event(0, 0x9E, 36, 100, 3001);
    shadow_dispatch_cable2_channeled_slots();

    CHECK(count(slot_calls, n_slot_calls, 0, MOVE_MIDI_SOURCE_FX_BROADCAST) == 1,
          "slot 0's audio FX must hear the external note");
    CHECK(count(slot_calls, n_slot_calls, 2, MOVE_MIDI_SOURCE_FX_BROADCAST) == 1,
          "slot 2's audio FX must hear the external note");
    CHECK(count(slot_calls, n_slot_calls, 1, MOVE_MIDI_SOURCE_FX_BROADCAST) == 0,
          "an empty slot must not be called");
}

static void test_no_double_broadcast_with_thru_slot(void)
{
    /* With a THRU slot, shadow_dispatch_direct_external_midi has already
     * broadcast this frame; doing it here too would fire every audio FX twice. */
    reset();
    load_slot(0, 0, SHADOW_FORWARD_AUTO);
    load_slot(1, -1, SHADOW_FORWARD_THRU);
    put_event(0, 0x9E, 36, 100, 4001);
    shadow_dispatch_cable2_channeled_slots();

    CHECK(count(slot_calls, n_slot_calls, -1, MOVE_MIDI_SOURCE_FX_BROADCAST) == 0,
          "with a THRU slot the channeled path must leave the broadcast to the THRU path");
    CHECK(n_mfx_calls == 0,
          "with a THRU slot the channeled path must leave the Master FX forward to the THRU path");
    CHECK(count(slot_calls, n_slot_calls, 1, MOVE_MIDI_SOURCE_EXTERNAL) == 0,
          "the THRU slot is served by the direct path, not this one");
}

int main(void)
{
    fake_api.api_version = 2;
    fake_api.on_midi = fake_on_midi;
    fake_api.get_param = fake_get_param;

    midi_host_t host;
    memset(&host, 0, sizeof(host));
    host.master_fx_forward_midi = fake_master_fx;
    host.chain_slots = slots;
    host.plugin_v2 = &api_ptr;
    host.shadow_control = &ctl_ptr;
    host.global_mmap_addr = &mmap_ptr;
    host.shadow_inprocess_ready = &inprocess_ready;
    host.shadow_display_mode = &display_mode;
    host.slot_idle = slot_idle;
    host.slot_silence_frames = slot_silence;
    host.slot_fx_idle = fx_idle;
    host.slot_fx_silence_frames = fx_silence;
    midi_routing_init(&host);

    test_unmatched_channel_reaches_fx();
    test_matched_channel_also_broadcast();
    test_broadcast_reaches_every_active_slot();
    test_no_double_broadcast_with_thru_slot();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: external MIDI reaches audio FX and Master FX, once, with or without a THRU slot\n");
    return 0;
}
