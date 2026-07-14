/* midi_net.h - MIDI over Wi-Fi service. */

#ifndef MIDI_NET_H
#define MIDI_NET_H

#include <stdint.h>

#include "shadow_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_NET_IPMIDI_GROUP    "225.0.0.37"
#define MIDI_NET_IPMIDI_PORT     21928
#define MIDI_NET_APPLE_CTRL_PORT 5004
#define MIDI_NET_APPLE_EVT_PORT  5005
#define MIDI_NET_MDNS_GROUP      "224.0.0.251"
#define MIDI_NET_MDNS_PORT       5353
#define MIDI_NET_SESSION_NAME    "Schwung-Move"

#define MIDI_NET_BACKEND_IPMIDI     0x01u
#define MIDI_NET_BACKEND_APPLEMIDI  0x02u
#define MIDI_NET_BACKEND_MDNS       0x04u
#define MIDI_NET_BACKEND_OUTBOUND   0x08u
#define MIDI_NET_BACKEND_ALL        0x0Fu

/* Cable 3 is an in-process origin tag used only inside the shared injection
 * ring. shadow_drain_midi_inject rewrites it to cable 2 before the packet can
 * reach Move. This lets the SPI-thread consumer mirror network-origin events
 * to an overtake UI without changing other cable-2 injection semantics. */
#define MIDI_NET_INJECT_CABLE 3u

typedef struct midi_net_stats_t {
    uint64_t rx_packets;
    uint64_t rx_midi_messages;
    uint64_t tx_packets;
    uint64_t drops_buffer_full;
    uint64_t drops_parse_err;
    uint64_t drops_outbound_full;
    uint32_t applemidi_peers;
    uint32_t mdns_queries;
    uint32_t mdns_responses;
} midi_net_stats_t;

/* Initialize once after the shim has created /schwung-midi-inject. */
void midi_net_init(shadow_midi_inject_t **inject_shm_ptr,
                   void (*log_fn)(const char *msg));
void midi_net_set_backends(uint32_t backend_flags);

/* Lifecycle calls are blocking and must run on a non-realtime thread. */
int  midi_net_start(void);
void midi_net_stop(void);
int  midi_net_is_running(void);

/* Realtime-safe, bounded, lock-free outbound enqueue. The packet is copied and
 * all socket I/O is deferred to the network thread. */
void midi_net_publish(const uint8_t pkt4[4]);

void midi_net_get_stats(midi_net_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_NET_H */
