/* shadow_link_audio.h - Link Audio interception, publishing, and channel reading
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_LINK_AUDIO_H
#define SHADOW_LINK_AUDIO_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "link_audio.h"
#include "shadow_constants.h"
#include "shadow_chain_types.h"

/* ============================================================================
 * Callback struct - shim functions link audio needs
 * ============================================================================ */

typedef struct {
    void (*log)(const char *msg);
    /* real sendto resolved by shim's sendto hook (for publisher thread) */
    ssize_t (**real_sendto_ptr)(int, const void *, size_t, int,
                                const struct sockaddr *, socklen_t);
    /* Shared state pointers */
    shadow_chain_slot_t *chain_slots;
} link_audio_host_t;

/* ============================================================================
 * Extern globals - link audio state readable/writable by the shim
 * ============================================================================ */

/* Global Link Audio state (types defined in link_audio.h) */
extern link_audio_state_t link_audio;

/* Stale packet tracking (updated by ioctl handler, read by monitor thread) */
extern uint32_t la_prev_intercepted;
extern uint32_t la_stale_frames;

/* Per-slot captured audio for publisher (written by render code, read by publisher) */
extern int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

/* ============================================================================
 * Inline byte-order helpers
 * ============================================================================ */

static inline uint32_t link_audio_read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint16_t link_audio_read_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline void link_audio_write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF;
    p[3] = v & 0xFF;
}

static inline void link_audio_write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

static inline void link_audio_write_u64_be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = v & 0xFF;
        v >>= 8;
    }
}

static inline int16_t link_audio_swap_i16(int16_t be_val)
{
    uint16_t u = (uint16_t)be_val;
    return (int16_t)(((u >> 8) & 0xFF) | ((u & 0xFF) << 8));
}

/* ============================================================================
 * Public functions
 * ============================================================================ */

/* Initialize link audio subsystem with callbacks to shim functions.
 * Must be called before any other link audio function. */
void shadow_link_audio_init(const link_audio_host_t *host);

/* Called from sendto() hook when a Link Audio packet is detected.
 * Handles both session announcements and audio data packets. */
void link_audio_on_sendto(int sockfd, const uint8_t *pkt, size_t len,
                          const struct sockaddr *dest_addr, socklen_t addrlen);

/* Read from a Move channel's ring buffer (called from consumer / render code).
 * Returns 1 if data was read, 0 if underrun. */
int link_audio_read_channel(int idx, int16_t *out, int frames);

/* Reset link audio state (called during link subscriber restart).
 * Clears session, channels, ring buffers, stale tracking. */
void link_audio_reset_state(void);

#endif /* SHADOW_LINK_AUDIO_H */
