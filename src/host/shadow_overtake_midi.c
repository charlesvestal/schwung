#include <string.h>

#include "shadow_midi_inject_writer.h"
#include "shadow_overtake_midi.h"

#define MIDI_IN_EVENT_STRIDE 8

static shadow_midi_inject_t overtake_midi_ring;

void shadow_overtake_midi_init(void)
{
    shadow_midi_inject_init(&overtake_midi_ring);
}

int shadow_overtake_midi_send(const uint8_t *msg, int len)
{
    if (!msg || len != 4) return 0;
    return shadow_midi_inject_push(&overtake_midi_ring, msg) == 0 ? 4 : 0;
}

static int drain_ring(shadow_midi_inject_t *ring,
                      uint8_t *midi_in,
                      int first_event,
                      int max_events)
{
    int copied = first_event;
    uint8_t packet[4];

    if (!ring) return copied;
    while (copied < max_events && shadow_midi_inject_peek(ring, packet)) {
        uint8_t *slot = &midi_in[copied * MIDI_IN_EVENT_STRIDE];
        if (slot[0] != 0) break;
        memcpy(slot, packet, 4);
        memset(slot + 4, 0, 4);
        shadow_midi_inject_pop(ring);
        copied++;
    }
    return copied;
}

int shadow_overtake_midi_drain(shadow_midi_inject_t *shared,
                               int overtake_active,
                               uint8_t *midi_in,
                               int max_events)
{
    int copied;

    if (!midi_in || max_events <= 0) return 0;
    copied = drain_ring(&overtake_midi_ring, midi_in, 0, max_events);
    if (!overtake_active)
        copied = drain_ring(shared, midi_in, copied, max_events);
    return copied;
}
