/*
 * SysEx Probe — the SLOT-MODULE half of the SysEx path.
 *
 * A chain slot reaches the outside world through a different door than a tool
 * does. A JS tool calls move_midi_external_send() and its packets travel the
 * shadow_ui SHM -> shadow_inject_ui_midi_out() path; a slot module calls
 * host->midi_send_external() and its packets go through the shim's
 * ROUTE_EXTERNAL ring. The two have different capacities, different overflow
 * behaviour, and until #307 the slot one was a NULL pointer. Testing one says
 * nothing about the other, which is why this module exists alongside the
 * sysex-test tool rather than instead of it.
 *
 * ---------------------------------------------------------------------------
 * BOTH RESULTS ARE READ ON THE MAC, deliberately.
 *
 * A counter this module keeps is only useful if something can read it, and
 * every readout available on the SPI callback is either an RT violation
 * (unified_log, any file I/O) or needs somebody looking at the OLED. So the
 * module answers on the wire instead:
 *
 *   TX  a note-on makes it emit a SysEx of a size chosen by velocity.
 *       Whatever the Mac receives IS the result.
 *
 *   RX  if process_midi() is ever handed an F0, it emits a short ECHO.
 *       The echo arriving proves a slot module can receive SysEx; silence
 *       proves it cannot.
 *
 * The predicted RX result is silence. Both cable-2 dispatchers
 * (shadow_dispatch_direct_external_midi, shadow_dispatch_cable2_channeled_slots)
 * gate on `cin < 0x08 || cin > 0x0E`, which excludes the SysEx CINs 0x04-0x07
 * before any channel filter runs — so a chain slot is write-only for SysEx.
 * This module is how that stops being a code-reading claim and becomes a
 * measurement. If the echo DOES arrive, the reading is wrong and the finding
 * has to be withdrawn.
 *
 * ---------------------------------------------------------------------------
 * REALTIME: process_midi() IS the SPI callback (see plugin_api_v1.h). Nothing
 * here allocates, logs, opens a file or takes a lock. midi_send_external() is
 * a lock-free ring push and is documented safe from exactly this context; its
 * 0 return means "dropped, retry" and is counted rather than assumed away.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../../../../host/plugin_api_v1.h"
#include "../../../../host/midi_fx_api_v1.h"

static const host_api_v1_t *g_host = NULL;

#define MFR 0x7D

/* Same four sizes as the sysex-test tool, so a result from one is directly
 * comparable with a result from the other. 158 is the QY-70 bulk-dump message
 * from #358. */
static const int SIZES[4] = { 64, 158, 316, 632 };

typedef struct {
    int  tx_messages;
    int  tx_packets;
    int  tx_refused;     /* midi_send_external() returned 0 */
    int  rx_sysex_bytes; /* payload bytes seen with the SysEx framing intact */
    int  rx_f0_seen;
    int  echo_pending;
} probe_t;

/* Body byte generator — byte-identical to the tool's bodyByte(), including the
 * aligned 00 00 00 run at 60..62 that #355 used to drop. Two senders, one
 * definition of what correct looks like. */
static inline uint8_t body_byte(int i)
{
    if (i >= 60 && i <= 62) return 0x00;
    return (uint8_t)(i & 0x7F);
}

/* Emit one SysEx as cable-2 USB-MIDI packets.
 *
 * Packets are pushed one at a time and a refusal is COUNTED, not ignored: the
 * ring is 64 packets and drop-newest, so a 158-byte message (53 packets) fits
 * but two back-to-back do not. A caller that treats the 0 as success reports a
 * message it never sent. */
static void emit_sysex(probe_t *p, int nbody)
{
    if (!g_host || !g_host->midi_send_external) return;

    uint8_t msg[8 + 632];
    int len = 0;
    msg[len++] = 0xF0;
    msg[len++] = MFR;
    msg[len++] = 0x02;                     /* 0x02 = from the slot module */
    for (int i = 0; i < nbody; i++) msg[len++] = body_byte(i);
    msg[len++] = 0xF7;

    int pos = 0, packets = 0;
    while (pos < len) {
        int remain = len - pos;
        uint8_t pkt[4];
        int take;
        if (remain > 3)        { pkt[0] = 0x24; take = 3; }
        else if (remain == 3)  { pkt[0] = 0x27; take = 3; }
        else if (remain == 2)  { pkt[0] = 0x26; take = 2; }
        else                   { pkt[0] = 0x25; take = 1; }
        pkt[1] = msg[pos];
        pkt[2] = take > 1 ? msg[pos + 1] : 0;
        pkt[3] = take > 2 ? msg[pos + 2] : 0;
        if (g_host->midi_send_external(pkt, 4) == 0) p->tx_refused++;
        pos += take;
        packets++;
    }
    p->tx_messages++;
    p->tx_packets = packets;
}

/* The RX proof: five packets saying "a slot module saw an F0". */
static void emit_echo(probe_t *p)
{
    if (!g_host || !g_host->midi_send_external) return;
    static const uint8_t echo[] = { 0xF0, MFR, 0x03, 0x45, 0x43, 0x48, 0x4F, 0xF7 };
    int pos = 0;
    while (pos < (int)sizeof(echo)) {
        int remain = (int)sizeof(echo) - pos;
        uint8_t pkt[4];
        int take;
        if (remain > 3)       { pkt[0] = 0x24; take = 3; }
        else if (remain == 3) { pkt[0] = 0x27; take = 3; }
        else if (remain == 2) { pkt[0] = 0x26; take = 2; }
        else                  { pkt[0] = 0x25; take = 1; }
        pkt[1] = echo[pos];
        pkt[2] = take > 1 ? echo[pos + 1] : 0;
        pkt[3] = take > 2 ? echo[pos + 2] : 0;
        g_host->midi_send_external(pkt, 4);
        pos += take;
    }
    p->echo_pending = 0;
}

static void *probe_create(const char *module_dir, const char *config_json)
{
    (void)module_dir; (void)config_json;
    probe_t *p = (probe_t *)calloc(1, sizeof(probe_t));
    return p;
}

static void probe_destroy(void *instance)
{
    free(instance);
}

static int probe_process_midi(void *instance,
                              const uint8_t *in_msg, int in_len,
                              uint8_t out_msgs[][3], int out_lens[],
                              int max_out)
{
    probe_t *p = (probe_t *)instance;
    if (!p || !in_msg || in_len < 1) return 0;

    /* --- RX side. If a SysEx byte ever reaches a slot module, say so. --- */
    for (int i = 0; i < in_len; i++) {
        if (in_msg[i] == 0xF0) { p->rx_f0_seen++; p->echo_pending = 1; }
        else if (p->rx_f0_seen && in_msg[i] < 0x80) p->rx_sysex_bytes++;
    }
    if (p->echo_pending) emit_echo(p);

    /* --- TX side. A note-on triggers a dump; velocity picks the size. --- */
    if ((in_msg[0] & 0xF0) == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        int idx = in_msg[2] / 32;          /* velocity 0-31,32-63,64-95,96-127 */
        if (idx > 3) idx = 3;
        emit_sysex(p, SIZES[idx]);
    }

    /* Pass everything through unchanged — this is a probe, not an effect. A
     * MIDI FX that swallowed notes would change what the rest of the chain
     * does and contaminate the interleaving test it exists to support. */
    if (max_out >= 1 && in_len <= 3) {
        for (int i = 0; i < in_len; i++) out_msgs[0][i] = in_msg[i];
        out_lens[0] = in_len;
        return 1;
    }
    return 0;
}

static int probe_tick(void *instance, int frames, int sample_rate,
                      uint8_t out_msgs[][3], int out_lens[], int max_out)
{
    (void)instance; (void)frames; (void)sample_rate;
    (void)out_msgs; (void)out_lens; (void)max_out;
    return 0;
}

static void probe_set_param(void *instance, const char *key, const char *val)
{
    probe_t *p = (probe_t *)instance;
    if (!p || !key || !val) return;
    /* A manual trigger, so the TX half can be exercised from the knob grid
     * with no MIDI flowing at all — which is the control condition for the
     * interleaving test. */
    if (strcmp(key, "send") == 0 && val[0] == '1') emit_sysex(p, SIZES[1]);
    else if (strcmp(key, "reset") == 0 && val[0] == '1') {
        p->tx_messages = p->tx_packets = p->tx_refused = 0;
        p->rx_sysex_bytes = p->rx_f0_seen = 0;
    }
}

static int probe_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    probe_t *p = (probe_t *)instance;
    if (!p || !key || !buf || buf_len < 2) return -1;
    if (strcmp(key, "tx_messages") == 0)    return snprintf(buf, buf_len, "%d", p->tx_messages);
    if (strcmp(key, "tx_packets") == 0)     return snprintf(buf, buf_len, "%d", p->tx_packets);
    if (strcmp(key, "tx_refused") == 0)     return snprintf(buf, buf_len, "%d", p->tx_refused);
    if (strcmp(key, "rx_f0_seen") == 0)     return snprintf(buf, buf_len, "%d", p->rx_f0_seen);
    if (strcmp(key, "rx_sysex_bytes") == 0) return snprintf(buf, buf_len, "%d", p->rx_sysex_bytes);
    if (strcmp(key, "send") == 0)           return snprintf(buf, buf_len, "0");
    if (strcmp(key, "reset") == 0)          return snprintf(buf, buf_len, "0");
    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version     = MIDI_FX_API_VERSION,
    .create_instance = probe_create,
    .destroy_instance = probe_destroy,
    .process_midi    = probe_process_midi,
    .tick            = probe_tick,
    .set_param       = probe_set_param,
    .get_param       = probe_get_param,
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host)
{
    g_host = host;
    return &g_api;
}
