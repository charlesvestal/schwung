/* shadow_link_audio.c - Link Audio interception, publishing, and channel reading
 * Extracted from move_anything_shim.c for maintainability.
 *
 * Move firmware 2.0 sends per-track audio over UDP/IPv6 using the "chnnlsv"
 * protocol.  This module intercepts those packets via the sendto() hook,
 * stores per-channel ring buffers for consumption by the DSP renderer, and
 * runs a publisher thread that sends shadow slot audio to Live. */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include "shadow_link_audio.h"
#include "shadow_chain_mgmt.h"

/* ============================================================================
 * Static host callbacks
 * ============================================================================ */

static link_audio_host_t host;
static int link_audio_initialized = 0;

/* ============================================================================
 * Global definitions
 * ============================================================================ */

link_audio_state_t link_audio;
uint32_t la_prev_intercepted = 0;
uint32_t la_stale_frames = 0;
int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

/* ============================================================================
 * Init
 * ============================================================================ */

void shadow_link_audio_init(const link_audio_host_t *h) {
    host = *h;
    memset(&link_audio, 0, sizeof(link_audio));
    link_audio.move_socket_fd = -1;
    link_audio.publisher_socket_fd = -1;
    memset(shadow_slot_capture, 0, sizeof(shadow_slot_capture));
    la_prev_intercepted = 0;
    la_stale_frames = 0;
    link_audio_initialized = 1;
}

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static void link_audio_parse_session(const uint8_t *pkt, size_t len,
                                     int sockfd, const struct sockaddr *dest,
                                     socklen_t addrlen);
static void link_audio_intercept_audio(const uint8_t *pkt);
static void *link_audio_publisher_thread_func(void *arg);
static void link_audio_start_publisher(void);

/* ============================================================================
 * sendto hook callback
 * ============================================================================ */

void link_audio_on_sendto(int sockfd, const uint8_t *pkt, size_t len,
                          const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (!link_audio_initialized) return;
    if (len < 12) return;

    if (memcmp(pkt, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN) != 0 ||
        pkt[7] != LINK_AUDIO_VERSION) {
        return;
    }

    uint8_t msg_type = pkt[8];
    if (msg_type == LINK_AUDIO_MSG_AUDIO && len == LINK_AUDIO_PACKET_SIZE) {
        link_audio_intercept_audio(pkt);
    } else if (msg_type == LINK_AUDIO_MSG_SESSION) {
        link_audio_parse_session(pkt, len, sockfd, dest_addr, addrlen);
    }
}

/* ============================================================================
 * Session parsing
 * ============================================================================ */

static void link_audio_parse_session(const uint8_t *pkt, size_t len,
                                     int sockfd, const struct sockaddr *dest,
                                     socklen_t addrlen) {
    if (len < 20) return;

    /* Copy Move's PeerID from offset 12 */
    memcpy(link_audio.move_peer_id, pkt + 12, 8);

    /* Capture network info for self-subscriber (first time only). */
    if (!link_audio.addr_captured && dest && dest->sa_family == AF_INET6) {
        link_audio.move_socket_fd = sockfd;
        memcpy(&link_audio.move_addr, dest, sizeof(struct sockaddr_in6));
        link_audio.move_addrlen = addrlen;

        socklen_t local_len = sizeof(link_audio.move_local_addr);
        if (getsockname(sockfd, (struct sockaddr *)&link_audio.move_local_addr,
                        &local_len) == 0) {
            /* Keep the port from getsockname — it's Move's bound/listening port */
        } else {
            memcpy(&link_audio.move_local_addr, dest, sizeof(struct sockaddr_in6));
        }

        link_audio.addr_captured = 1;

        if (link_audio.session_parsed && !link_audio.publisher_running) {
            link_audio_start_publisher();
        }

        /* Write Move's chnnlsv endpoint to file for standalone link-subscriber */
        {
            char local_str_ep[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &link_audio.move_local_addr.sin6_addr,
                      local_str_ep, sizeof(local_str_ep));
            FILE *ep = fopen("/data/UserData/move-anything/link-audio-endpoint", "w");
            if (ep) {
                fprintf(ep, "%s %d %u\n",
                        local_str_ep,
                        ntohs(link_audio.move_local_addr.sin6_port),
                        link_audio.move_local_addr.sin6_scope_id);
                fclose(ep);
            }
        }

        char dest_str[INET6_ADDRSTRLEN], local_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &link_audio.move_addr.sin6_addr,
                  dest_str, sizeof(dest_str));
        inet_ntop(AF_INET6, &link_audio.move_local_addr.sin6_addr,
                  local_str, sizeof(local_str));
        char logbuf[512];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: captured dest=%s:%d, local(Move)=%s:%d scope=%d",
                 dest_str, ntohs(link_audio.move_addr.sin6_port),
                 local_str, ntohs(link_audio.move_local_addr.sin6_port),
                 link_audio.move_local_addr.sin6_scope_id);
        if (host.log) host.log(logbuf);
    }

    /* Parse TLV entries starting at offset 20 */
    size_t pos = 20;
    while (pos + 8 <= len) {
        const uint8_t *tag = pkt + pos;
        uint32_t tlen = link_audio_read_u32_be(pkt + pos + 4);
        pos += 8;

        if (pos + tlen > len) break;

        if (memcmp(tag, "sess", 4) == 0 && tlen == 8) {
            memcpy(link_audio.session_id, pkt + pos, 8);

        } else if (memcmp(tag, "auca", 4) == 0 && tlen >= 4) {
            const uint8_t *auca = pkt + pos;
            size_t auca_end = tlen;
            uint32_t num_channels = link_audio_read_u32_be(auca);
            size_t auca_pos = 4;

            int count = 0;
            for (uint32_t c = 0; c < num_channels && auca_pos + 4 <= auca_end; c++) {
                uint32_t name_len = link_audio_read_u32_be(auca + auca_pos);
                auca_pos += 4;
                if (auca_pos + name_len + 8 > auca_end) break;

                if (count < LINK_AUDIO_MOVE_CHANNELS) {
                    link_audio_channel_t *ch = &link_audio.channels[count];
                    int nlen = name_len < 31 ? name_len : 31;
                    memcpy(ch->name, auca + auca_pos, nlen);
                    ch->name[nlen] = '\0';
                    auca_pos += name_len;
                    memcpy(ch->channel_id, auca + auca_pos, 8);
                    auca_pos += 8;
                    ch->active = 1;
                    count++;
                } else {
                    auca_pos += name_len + 8;
                }
            }
            link_audio.move_channel_count = count;
        }

        pos += tlen;
    }

    if (!link_audio.session_parsed &&
        link_audio.move_channel_count > 0) {
        link_audio.session_parsed = 1;
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: session parsed, %d channels discovered",
                 link_audio.move_channel_count);
        if (host.log) host.log(logbuf);
        for (int i = 0; i < link_audio.move_channel_count; i++) {
            char ch_log[128];
            snprintf(ch_log, sizeof(ch_log), "Link Audio:   [%d] \"%s\"",
                     i, link_audio.channels[i].name);
            if (host.log) host.log(ch_log);
        }

        if (link_audio.addr_captured) {
            link_audio_start_publisher();
        }
    }
}

/* ============================================================================
 * Audio interception (runs on audio thread — must be fast)
 * ============================================================================ */

static void link_audio_intercept_audio(const uint8_t *pkt) {
    const uint8_t *channel_id = pkt + 20;

    int idx = -1;
    for (int i = 0; i < link_audio.move_channel_count; i++) {
        if (memcmp(link_audio.channels[i].channel_id, channel_id, 8) == 0) {
            idx = i;
            break;
        }
    }

    /* Auto-discover channels from audio packets */
    if (idx < 0 && link_audio.move_channel_count < LINK_AUDIO_MOVE_CHANNELS) {
        idx = link_audio.move_channel_count;
        link_audio_channel_t *ch = &link_audio.channels[idx];
        memcpy(ch->channel_id, channel_id, 8);
        snprintf(ch->name, sizeof(ch->name), "ch%d", idx);
        ch->active = 1;
        ch->write_pos = 0;
        ch->read_pos = 0;
        ch->peak = 0;
        ch->pkt_count = 0;
        link_audio.move_channel_count = idx + 1;

        memcpy(link_audio.move_peer_id, pkt + 12, 8);

        char logbuf[128];
        snprintf(logbuf, sizeof(logbuf),
                 "Link Audio: auto-discovered channel %d (id %02x%02x%02x%02x%02x%02x%02x%02x)",
                 idx, channel_id[0], channel_id[1], channel_id[2], channel_id[3],
                 channel_id[4], channel_id[5], channel_id[6], channel_id[7]);
        if (host.log) host.log(logbuf);
    }

    if (idx < 0) return;

    link_audio_channel_t *ch = &link_audio.channels[idx];

    const int16_t *src = (const int16_t *)(pkt + LINK_AUDIO_HEADER_SIZE);
    uint32_t wp = ch->write_pos;
    uint32_t rp = ch->read_pos;
    int samples_to_write = LINK_AUDIO_FRAMES_PER_PACKET * 2;

    if ((wp - rp) + (uint32_t)samples_to_write > LINK_AUDIO_RING_SAMPLES) {
        link_audio.overruns++;
        return;
    }

    int peak = ch->peak;

    for (int i = 0; i < samples_to_write; i++) {
        int16_t sample = link_audio_swap_i16(src[i]);
        ch->ring[wp & LINK_AUDIO_RING_MASK] = sample;
        wp++;
        int abs_s = (sample < 0) ? -(int)sample : (int)sample;
        if (abs_s > peak) peak = abs_s;
    }

    __sync_synchronize();
    ch->write_pos = wp;
    ch->peak = (int16_t)(peak > 32767 ? 32767 : peak);
    ch->pkt_count++;

    ch->sequence = link_audio_read_u32_be(pkt + 44);

    link_audio.packets_intercepted++;
}

/* ============================================================================
 * Channel reading (called from consumer / render code)
 * ============================================================================ */

int link_audio_read_channel(int idx, int16_t *out, int frames) {
    if (idx < 0 || idx >= link_audio.move_channel_count) return 0;

    link_audio_channel_t *ch = &link_audio.channels[idx];
    int samples = frames * 2;

    __sync_synchronize();
    uint32_t rp = ch->read_pos;
    uint32_t wp = ch->write_pos;
    uint32_t avail = wp - rp;

    if (avail < (uint32_t)samples) {
        memset(out, 0, samples * sizeof(int16_t));
        link_audio.underruns++;
        return 0;
    }

    if (avail > (uint32_t)samples * 4) {
        rp = wp - (uint32_t)samples;
    }

    for (int i = 0; i < samples; i++) {
        out[i] = ch->ring[rp & LINK_AUDIO_RING_MASK];
        rp++;
    }

    __sync_synchronize();
    ch->read_pos = rp;
    return 1;
}

/* ============================================================================
 * Publisher
 * ============================================================================ */

static void link_audio_start_publisher(void) {
    /* Publisher disabled on main: needs Link SDK integration */
    return;
}

static int link_audio_build_session_announcement(uint8_t *pkt, int max_len) {
    (void)max_len;
    int pos = 0;

    memcpy(pkt + pos, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN);
    pos += LINK_AUDIO_MAGIC_LEN;
    pkt[pos++] = LINK_AUDIO_VERSION;
    pkt[pos++] = LINK_AUDIO_MSG_SESSION;
    pkt[pos++] = 0;
    pkt[pos++] = 0;
    pkt[pos++] = 0;

    memcpy(pkt + pos, link_audio.publisher_peer_id, 8);
    pos += 8;

    /* TLV: "sess" */
    memcpy(pkt + pos, "sess", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 8); pos += 4;
    memcpy(pkt + pos, link_audio.publisher_session_id, 8); pos += 8;

    /* TLV: "__pi" */
    const char *peer_name = "ME";
    uint32_t name_len = (uint32_t)strlen(peer_name);
    memcpy(pkt + pos, "__pi", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 4 + name_len); pos += 4;
    link_audio_write_u32_be(pkt + pos, name_len); pos += 4;
    memcpy(pkt + pos, peer_name, name_len); pos += name_len;

    /* TLV: "auca" */
    int active_count = 0;
    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (host.chain_slots && host.chain_slots[i].active) active_count++;
    }

    uint32_t auca_size = 4;
    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (!host.chain_slots || !host.chain_slots[i].active) continue;
        auca_size += 4 + (uint32_t)strlen(link_audio.pub_channels[i].name) + 8;
    }

    memcpy(pkt + pos, "auca", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, auca_size); pos += 4;
    link_audio_write_u32_be(pkt + pos, active_count); pos += 4;

    for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
        if (!host.chain_slots || !host.chain_slots[i].active) continue;
        uint32_t ch_name_len = (uint32_t)strlen(link_audio.pub_channels[i].name);
        link_audio_write_u32_be(pkt + pos, ch_name_len); pos += 4;
        memcpy(pkt + pos, link_audio.pub_channels[i].name, ch_name_len);
        pos += ch_name_len;
        memcpy(pkt + pos, link_audio.pub_channels[i].channel_id, 8);
        pos += 8;
    }

    /* TLV: "__ht" */
    memcpy(pkt + pos, "__ht", 4); pos += 4;
    link_audio_write_u32_be(pkt + pos, 8); pos += 4;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ts = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    link_audio_write_u64_be(pkt + pos, ts); pos += 8;

    return pos;
}

static void link_audio_build_audio_packet(uint8_t *pkt,
                                          const uint8_t *peer_id,
                                          const uint8_t *channel_id,
                                          uint32_t sequence,
                                          const int16_t *samples_le,
                                          int num_frames) {
    memset(pkt, 0, LINK_AUDIO_PACKET_SIZE);

    memcpy(pkt, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN);
    pkt[7] = LINK_AUDIO_VERSION;
    pkt[8] = LINK_AUDIO_MSG_AUDIO;

    memcpy(pkt + 12, peer_id, 8);
    memcpy(pkt + 20, channel_id, 8);
    memcpy(pkt + 28, peer_id, 8);

    link_audio_write_u32_be(pkt + 36, 1);
    link_audio_write_u32_be(pkt + 44, sequence);
    link_audio_write_u16_be(pkt + 48, (uint16_t)num_frames);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ts = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    link_audio_write_u64_be(pkt + 52, ts);
    link_audio_write_u32_be(pkt + 60, 6);
    pkt[64] = 0xd5; pkt[65] = 0x11; pkt[66] = 0x01;
    link_audio_write_u32_be(pkt + 67, 44100);
    pkt[71] = 2;
    link_audio_write_u16_be(pkt + 72, LINK_AUDIO_PAYLOAD_SIZE);

    int16_t *dst = (int16_t *)(pkt + LINK_AUDIO_HEADER_SIZE);
    for (int i = 0; i < num_frames * 2; i++) {
        dst[i] = link_audio_swap_i16(samples_le[i]);
    }
}

static void *link_audio_publisher_thread_func(void *arg) {
    (void)arg;

    ssize_t (*do_sendto)(int, const void *, size_t, int,
                         const struct sockaddr *, socklen_t) = NULL;
    if (host.real_sendto_ptr) do_sendto = *host.real_sendto_ptr;
    if (!do_sendto) {
        if (host.log) host.log("Link Audio: publisher has no sendto, exiting");
        return NULL;
    }

    struct sockaddr_in6 dest_addr;
    memcpy(&dest_addr, &link_audio.move_addr, sizeof(dest_addr));

    uint8_t session_pkt[512];
    uint8_t audio_pkt[LINK_AUDIO_PACKET_SIZE];
    uint8_t recv_buf[128];

    uint32_t tick_counter = 0;

    int16_t accum[LINK_AUDIO_SHADOW_CHANNELS][LINK_AUDIO_PUB_RING_SAMPLES];
    uint32_t accum_wp[LINK_AUDIO_SHADOW_CHANNELS];
    uint32_t accum_rp[LINK_AUDIO_SHADOW_CHANNELS];
    memset(accum_wp, 0, sizeof(accum_wp));
    memset(accum_rp, 0, sizeof(accum_rp));
    memset(accum, 0, sizeof(accum));

    while (link_audio.publisher_running && link_audio.enabled) {
        while (!link_audio.publisher_tick && link_audio.publisher_running) {
            struct timespec ts = {0, 500000L};
            nanosleep(&ts, NULL);
        }
        link_audio.publisher_tick = 0;
        tick_counter++;

        /* Session announcement every ~1 second */
        if (tick_counter % 344 == 0) {
            int pkt_len = link_audio_build_session_announcement(session_pkt,
                                                                sizeof(session_pkt));
            do_sendto(link_audio.publisher_socket_fd, session_pkt, pkt_len, 0,
                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        }

        /* Check for incoming ChannelRequests */
        struct sockaddr_in6 from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(link_audio.publisher_socket_fd, recv_buf,
                             sizeof(recv_buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n >= 36 && memcmp(recv_buf, LINK_AUDIO_MAGIC, LINK_AUDIO_MAGIC_LEN) == 0 &&
            recv_buf[8] == LINK_AUDIO_MSG_REQUEST) {
            for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
                if (memcmp(recv_buf + 20,
                           link_audio.pub_channels[i].channel_id, 8) == 0) {
                    link_audio.pub_channels[i].subscribed = 1;
                    memcpy(&dest_addr, &from_addr, sizeof(dest_addr));
                }
            }
        }

        /* Feed captured slot audio into accumulators */
        for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
            if (!host.chain_slots || !host.chain_slots[i].active) continue;

            uint32_t wp = accum_wp[i];
            for (int s = 0; s < FRAMES_PER_BLOCK * 2; s++) {
                accum[i][wp & LINK_AUDIO_PUB_RING_MASK] = shadow_slot_capture[i][s];
                wp++;
            }
            accum_wp[i] = wp;
        }

        /* Drain 125-frame packets */
        for (int i = 0; i < LINK_AUDIO_SHADOW_CHANNELS; i++) {
            if (!link_audio.pub_channels[i].subscribed) continue;
            if (!host.chain_slots || !host.chain_slots[i].active) continue;

            uint32_t avail = accum_wp[i] - accum_rp[i];
            while (avail >= LINK_AUDIO_FRAMES_PER_PACKET * 2) {
                int16_t out_frames[LINK_AUDIO_FRAMES_PER_PACKET * 2];
                uint32_t rp = accum_rp[i];
                for (int s = 0; s < LINK_AUDIO_FRAMES_PER_PACKET * 2; s++) {
                    out_frames[s] = accum[i][rp & LINK_AUDIO_PUB_RING_MASK];
                    rp++;
                }
                accum_rp[i] = rp;

                link_audio_build_audio_packet(audio_pkt,
                                              link_audio.publisher_peer_id,
                                              link_audio.pub_channels[i].channel_id,
                                              link_audio.pub_channels[i].sequence++,
                                              out_frames,
                                              LINK_AUDIO_FRAMES_PER_PACKET);
                do_sendto(link_audio.publisher_socket_fd, audio_pkt,
                          LINK_AUDIO_PACKET_SIZE, 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                link_audio.packets_published++;

                avail = accum_wp[i] - accum_rp[i];
            }
        }
    }

    close(link_audio.publisher_socket_fd);
    link_audio.publisher_socket_fd = -1;
    if (host.log) host.log("Link Audio: publisher thread exited");
    return NULL;
}

/* ============================================================================
 * State reset (called during link subscriber restart)
 * ============================================================================ */

void link_audio_reset_state(void) {
    link_audio.packets_intercepted = 0;
    link_audio.session_parsed = 0;
    link_audio.move_channel_count = 0;
    la_prev_intercepted = 0;
    la_stale_frames = 0;

    for (int i = 0; i < LINK_AUDIO_MOVE_CHANNELS; i++) {
        link_audio.channels[i].write_pos = 0;
        link_audio.channels[i].read_pos = 0;
        link_audio.channels[i].active = 0;
        link_audio.channels[i].pkt_count = 0;
        link_audio.channels[i].peak = 0;
    }
}
