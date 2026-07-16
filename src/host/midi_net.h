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
#define MIDI_NET_SESSION_NAME    "Schwung-Move"

/* Cable 3 is an in-process origin tag used only inside the shared injection
 * ring. shadow_drain_midi_inject rewrites it to cable 2 before the packet can
 * reach Move. This lets the SPI-thread consumer mirror network-origin events
 * to an overtake UI without changing other cable-2 injection semantics. */
#define MIDI_NET_INJECT_CABLE 3u

/* Initialize once after the shim has created /schwung-midi-inject. */
void midi_net_init(shadow_midi_inject_t **inject_shm_ptr);

/* Start or stop the service to match enabled. Blocking; worker-thread only. */
void midi_net_reconcile(int enabled);

/* Realtime-safe, bounded, lock-free outbound enqueue. The packet is copied and
 * all socket I/O is deferred to the network thread. */
void midi_net_publish(const uint8_t pkt4[4]);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_NET_H */
