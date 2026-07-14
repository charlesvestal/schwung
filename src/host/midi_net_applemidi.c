/*
 * midi_net_applemidi.c - AppleMIDI (RTP-MIDI, RFC 6295) backend
 *
 * Implements inbound + outbound AppleMIDI session support over two UDP
 * sockets (control port + event port). Parses incoming RTP-MIDI command
 * sections and injects them into Move's MIDI_IN via the shared injector.
 *
 * Session negotiation:
 *   1. Peer sends IN (Invitation) to control port 5004
 *   2. We reply OK (accept) with our SSRC
 *   3. Peer sends IN to event port 5005
 *   4. We reply OK
 *   5. Three CK (clock sync) rounds establish timing
 *   6. MIDI packets flow on event port
 *
 * The journal (recovery from lost packets) is skipped for inbound — we
 * rely on UDP being reliable enough on a LAN. Outbound has no journal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "midi_net.h"
#include "midi_net_internal.h"

/* ============================================================================
 * AppleMIDI protocol constants
 * ============================================================================ */

#define APPLE_SIGNATURE      0xFFFF  /* First 2 bytes of session commands */
#define APPLE_PROTOCOL_VER   2

/* Session control commands (at offset 2, 2 bytes) */
#define APPLE_CMD_IN  0x494E  /* "IN" - Invitation */
#define APPLE_CMD_OK  0x4F4B  /* "OK" - Accept */
#define APPLE_CMD_NO  0x4E4F  /* "NO" - Reject */
#define APPLE_CMD_BY  0x4259  /* "BY" - Bye */
#define APPLE_CMD_CK  0x434B  /* "CK" - Clock sync */
#define APPLE_CMD_RS  0x5253  /* "RS" - Receiver Feedback */

/* RTP header bytes */
#define RTP_VERSION_MASK  0xC0
#define RTP_VERSION_2     0x80
#define RTP_PAYLOAD_TYPE  0x61  /* RTP-MIDI payload type (97) */

/* ============================================================================
 * Byte-order helpers
 * ============================================================================ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v & 0xFF;
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

/* ============================================================================
 * Peer table helpers
 * ============================================================================ */

static int same_ipv4_host(const struct sockaddr_storage *known,
                          socklen_t known_len,
                          const struct sockaddr_in *from) {
    if (!known_len || known->ss_family != AF_INET) return 0;
    const struct sockaddr_in *addr = (const struct sockaddr_in *)known;
    return addr->sin_addr.s_addr == from->sin_addr.s_addr;
}

static midi_net_peer_t *find_peer_by_token(uint32_t token,
                                           const struct sockaddr_in *from) {
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        midi_net_peer_t *peer = &g_midi_net.peers[i];
        if (peer->active && peer->initiator_token == token &&
            same_ipv4_host(&peer->ctrl_addr, peer->ctrl_addrlen, from)) {
            return &g_midi_net.peers[i];
        }
    }
    return NULL;
}

static midi_net_peer_t *find_peer_by_ssrc(uint32_t ssrc) {
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        if (g_midi_net.peers[i].active && g_midi_net.peers[i].peer_ssrc == ssrc) {
            return &g_midi_net.peers[i];
        }
    }
    return NULL;
}

static midi_net_peer_t *alloc_peer(void) {
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        if (!g_midi_net.peers[i].active) {
            memset(&g_midi_net.peers[i], 0, sizeof(midi_net_peer_t));
            return &g_midi_net.peers[i];
        }
    }
    return NULL;
}

static void free_peer(midi_net_peer_t *peer) {
    if (!peer) return;
    memset(peer, 0, sizeof(*peer));
}

static void update_peers_stat(void) {
    uint32_t n = 0;
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        if (g_midi_net.peers[i].active >= 3) n++;
    }
    midi_net_stat_store_u32(&g_midi_net.stats.applemidi_peers, n);
}

static int same_ipv4_endpoint(const struct sockaddr_storage *known,
                              socklen_t known_len,
                              const struct sockaddr_in *from) {
    if (!known_len || known->ss_family != AF_INET) return 0;
    const struct sockaddr_in *addr = (const struct sockaddr_in *)known;
    return addr->sin_addr.s_addr == from->sin_addr.s_addr &&
           addr->sin_port == from->sin_port;
}

static int peer_matches_endpoint(const midi_net_peer_t *peer, int on_ctrl,
                                 const struct sockaddr_in *from) {
    return on_ctrl
        ? same_ipv4_endpoint(&peer->ctrl_addr, peer->ctrl_addrlen, from)
        : same_ipv4_endpoint(&peer->evt_addr, peer->evt_addrlen, from);
}

/* ============================================================================
 * Session command builder
 * ============================================================================ */

static int build_session_command(uint8_t *out, int max_len,
                                  uint16_t cmd, uint32_t token,
                                  uint32_t ssrc, const char *name) {
    if (max_len < 16) return 0;
    wr16(out, APPLE_SIGNATURE);
    wr16(out + 2, cmd);
    wr32(out + 4, APPLE_PROTOCOL_VER);
    wr32(out + 8, token);
    wr32(out + 12, ssrc);
    int len = 16;
    if (name && *name) {
        int nlen = (int)strlen(name);
        if (len + nlen + 1 > max_len) nlen = max_len - len - 1;
        if (nlen < 0) nlen = 0;
        memcpy(out + len, name, nlen);
        out[len + nlen] = 0;
        len += nlen + 1;
    }
    return len;
}

static int build_ck_reply(uint8_t *out, int max_len,
                          uint32_t ssrc, uint8_t count,
                          uint64_t ts1, uint64_t ts2, uint64_t ts3) {
    if (max_len < 36) return 0;
    wr16(out, APPLE_SIGNATURE);
    wr16(out + 2, APPLE_CMD_CK);
    wr32(out + 4, ssrc);
    out[8] = count;
    out[9] = 0; out[10] = 0; out[11] = 0;
    wr64(out + 12, ts1);
    wr64(out + 20, ts2);
    wr64(out + 28, ts3);
    return 36;
}

/* ============================================================================
 * Send helpers
 * ============================================================================ */

static int send_to_evt(midi_net_peer_t *peer, const uint8_t *buf, int len) {
    if (!peer || peer->evt_addrlen == 0) return 0;
    ssize_t sent = sendto(g_midi_net.applemidi_evt_sock, buf, len, 0,
                          (struct sockaddr *)&peer->evt_addr, peer->evt_addrlen);

    static int send_error_count = 0;
    if (sent != len && send_error_count < 10) {
        char addr[INET_ADDRSTRLEN] = "?";
        uint16_t port = 0;
        if (peer->evt_addr.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&peer->evt_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr));
            port = ntohs(sin->sin_port);
        }
        if (sent < 0) {
            midi_net_log("AppleMIDI send evt failed: dst=%s:%u len=%d errno=%d %s",
                         addr, port, len, errno, strerror(errno));
        } else {
            midi_net_log("AppleMIDI send evt partial: dst=%s:%u len=%d sent=%zd",
                         addr, port, len, sent);
        }
        send_error_count++;
    }

    return sent == len;
}

/* ============================================================================
 * Socket lifecycle
 * ============================================================================ */

static int open_udp(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

int midi_net_applemidi_open(void) {
    int ctrl = open_udp(MIDI_NET_APPLE_CTRL_PORT);
    if (ctrl < 0) {
        midi_net_log("AppleMIDI: bind control :%d failed: %s",
                     MIDI_NET_APPLE_CTRL_PORT, strerror(errno));
        return -1;
    }
    int evt = open_udp(MIDI_NET_APPLE_EVT_PORT);
    if (evt < 0) {
        midi_net_log("AppleMIDI: bind event :%d failed: %s",
                     MIDI_NET_APPLE_EVT_PORT, strerror(errno));
        close(ctrl);
        return -1;
    }
    g_midi_net.applemidi_ctrl_sock = ctrl;
    g_midi_net.applemidi_evt_sock = evt;
    return 0;
}

void midi_net_applemidi_close(void) {
    if (g_midi_net.applemidi_ctrl_sock >= 0) {
        close(g_midi_net.applemidi_ctrl_sock);
        g_midi_net.applemidi_ctrl_sock = -1;
    }
    if (g_midi_net.applemidi_evt_sock >= 0) {
        close(g_midi_net.applemidi_evt_sock);
        g_midi_net.applemidi_evt_sock = -1;
    }
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        free_peer(&g_midi_net.peers[i]);
    }
    update_peers_stat();
}

/* ============================================================================
 * Control port handler (session negotiation + CK sync)
 * ============================================================================ */

static void handle_session_command(int on_ctrl, const uint8_t *buf, int n,
                                    struct sockaddr_in *from, socklen_t fromlen) {
    if (n < 4) return;
    if (rd16(buf) != APPLE_SIGNATURE) return;

    uint16_t cmd = rd16(buf + 2);

    if (cmd == APPLE_CMD_IN) {
        if (n < 16) return;
        uint32_t token = rd32(buf + 8);
        uint32_t peer_ssrc = rd32(buf + 12);
        /* Name is optional, nul-terminated after offset 16 */

        midi_net_peer_t *peer = find_peer_by_token(token, from);
        if (!peer) {
            peer = alloc_peer();
            if (!peer) {
                /* Table full — reply NO */
                uint8_t reply[16];
                int rlen = build_session_command(reply, sizeof(reply),
                                                 APPLE_CMD_NO, token,
                                                 g_midi_net.our_ssrc, NULL);
                sendto(on_ctrl ? g_midi_net.applemidi_ctrl_sock
                               : g_midi_net.applemidi_evt_sock,
                       reply, rlen, 0, (struct sockaddr *)from, fromlen);
                midi_net_log("AppleMIDI: peer table full, rejecting IN");
                return;
            }
            peer->initiator_token = token;
            peer->peer_ssrc = peer_ssrc;
            peer->ssrc = g_midi_net.our_ssrc + (uint32_t)(peer - g_midi_net.peers) + 1;
        } else if (peer->peer_ssrc != peer_ssrc) {
            midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
            return;
        }

        if (on_ctrl) {
            memcpy(&peer->ctrl_addr, from, fromlen);
            peer->ctrl_addrlen = fromlen;
            if (peer->active < 1) peer->active = 1;
            midi_net_log("AppleMIDI: IN (ctrl) from ssrc=%08x token=%08x", peer_ssrc, token);
        } else {
            memcpy(&peer->evt_addr, from, fromlen);
            peer->evt_addrlen = fromlen;
            if (peer->active < 2) peer->active = 2;
            midi_net_log("AppleMIDI: IN (evt) from ssrc=%08x", peer_ssrc);
        }
        peer->last_activity_ms = midi_net_now_ms();

        /* Reply OK */
        uint8_t reply[64];
        int rlen = build_session_command(reply, sizeof(reply),
                                         APPLE_CMD_OK, token,
                                         peer->ssrc, MIDI_NET_SESSION_NAME);
        sendto(on_ctrl ? g_midi_net.applemidi_ctrl_sock
                       : g_midi_net.applemidi_evt_sock,
               reply, rlen, 0, (struct sockaddr *)from, fromlen);
        update_peers_stat();
        return;
    }

    if (cmd == APPLE_CMD_BY) {
        if (n < 16) return;
        uint32_t peer_ssrc = rd32(buf + 12);
        midi_net_peer_t *peer = find_peer_by_ssrc(peer_ssrc);
        if (peer && peer_matches_endpoint(peer, on_ctrl, from)) {
            midi_net_log("AppleMIDI: BY from ssrc=%08x", peer_ssrc);
            free_peer(peer);
            update_peers_stat();
        }
        return;
    }

    if (cmd == APPLE_CMD_CK) {
        if (n < 36) return;
        uint32_t peer_ssrc = rd32(buf + 4);
        uint8_t  count     = buf[8];
        uint64_t ts1 = rd64(buf + 12);
        uint64_t ts2 = rd64(buf + 20);
        uint64_t ts3 = rd64(buf + 28);
        (void)ts3;

        midi_net_peer_t *peer = find_peer_by_ssrc(peer_ssrc);
        if (!peer || !peer_matches_endpoint(peer, on_ctrl, from)) return;

        peer->last_activity_ms = midi_net_now_ms();

        uint64_t now_ticks = midi_net_now_100us();
        uint8_t reply[64];
        int rlen = 0;

        if (count == 0) {
            /* Peer sent CK0 (their ts1). We reply with CK1 including our ts2. */
            rlen = build_ck_reply(reply, sizeof(reply), peer->ssrc, 1,
                                  ts1, now_ticks, 0);
            sendto(on_ctrl ? g_midi_net.applemidi_ctrl_sock
                           : g_midi_net.applemidi_evt_sock,
                   reply, rlen, 0, (struct sockaddr *)from, fromlen);
        } else if (count == 1) {
            /* Peer sent CK1 (ts1, ts2). We reply CK2 with ts3. */
            rlen = build_ck_reply(reply, sizeof(reply), peer->ssrc, 2,
                                  ts1, ts2, now_ticks);
            sendto(on_ctrl ? g_midi_net.applemidi_ctrl_sock
                           : g_midi_net.applemidi_evt_sock,
                   reply, rlen, 0, (struct sockaddr *)from, fromlen);
            peer->ck_count++;
            if (peer->ck_count >= 1 && peer->active < 3) {
                peer->active = 3;  /* ESTABLISHED */
                midi_net_log("AppleMIDI: session established for ssrc=%08x", peer_ssrc);
                update_peers_stat();
            }
        } else if (count == 2) {
            /* CK2 final — no reply. */
            peer->ck_count++;
            if (peer->active < 3) {
                peer->active = 3;
                midi_net_log("AppleMIDI: session established for ssrc=%08x", peer_ssrc);
                update_peers_stat();
            }
        }
        return;
    }

    if (cmd == APPLE_CMD_RS) {
        /* Receiver feedback — we don't implement recovery journal, so ignore. */
        return;
    }

    /* Unknown command — ignore */
}

/* ============================================================================
 * RTP-MIDI event packet parser
 *
 * Layout:
 *   [0]   V(2) P(1) X(1) CC(4)
 *   [1]   M(1) PT(7)
 *   [2-3] sequence number
 *   [4-7] timestamp
 *   [8-11] SSRC
 *   [12]  command section header
 *     - Flag B (bit 7): 1 = 12-bit length, 0 = 4-bit length
 *     - Flag J (bit 6): journal present
 *     - Flag Z (bit 5): Delta-time present before first command
 *     - Flag P (bit 4): command has initial status byte omitted (running status)
 *     - Low 4 bits (or next byte): length of MIDI command section
 *   [13..] MIDI commands (each optionally preceded by delta-time)
 *   [journal...] skipped
 * ============================================================================ */

/* Decode a variable-length delta-time. Returns number of bytes consumed.
 * MIDI delta-time uses the standard variable-length quantity (VLQ). */
static int decode_vlq(const uint8_t *p, int avail, uint32_t *out) {
    uint32_t v = 0;
    int consumed = 0;
    while (consumed < 4 && consumed < avail) {
        uint8_t b = p[consumed++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) {
            if (out) *out = v;
            return consumed;
        }
    }
    return -1;
}

static void parse_rtp_midi_packet(midi_net_peer_t *peer, const uint8_t *buf, int n) {
    if (n < 13) return;

    if ((buf[0] & RTP_VERSION_MASK) != RTP_VERSION_2) return;
    /* This implementation expects the RFC 6295 base RTP header: no CSRC list
     * and no RTP extension. Reject instead of parsing the command header at
     * the wrong offset. */
    if ((buf[0] & 0x1F) != 0) return;
    if ((buf[1] & 0x7F) != RTP_PAYLOAD_TYPE) return;

    uint32_t ssrc = rd32(buf + 8);
    if (peer && peer->peer_ssrc && peer->peer_ssrc != ssrc) return;

    int off = 12;
    uint8_t h = buf[off++];
    int has_journal = (h & 0x40) != 0;
    int has_delta_first = (h & 0x20) != 0;
    (void)has_delta_first;
    int running_status_from_previous = (h & 0x10) != 0;

    int cmd_len;
    if (h & 0x80) {
        /* 12-bit length */
        if (off >= n) return;
        cmd_len = ((h & 0x0F) << 8) | buf[off++];
    } else {
        cmd_len = h & 0x0F;
    }

    if (off + cmd_len > n) {
        midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
        return;
    }
    if (cmd_len <= 0) return;

    /* Skip initial delta-time if present */
    int cmd_off = off;
    int cmd_end = off + cmd_len;

    /* Walk the command section: [delta-time] [MIDI] [delta-time] [MIDI] ...
     * First command has a delta-time only if Z flag set. Subsequent always do.
     */
    int first = 1;
    uint8_t last_status =
        (running_status_from_previous && peer) ? peer->last_status : 0;

    if (running_status_from_previous && !last_status) {
        midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
        return;
    }

    while (cmd_off < cmd_end) {
        if (!first || has_delta_first) {
            uint32_t dt = 0;
            int vlq = decode_vlq(buf + cmd_off, cmd_end - cmd_off, &dt);
            if (vlq < 0) {
                midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
                break;
            }
            cmd_off += vlq;
            (void)dt;
        }
        first = 0;

        if (cmd_off >= cmd_end) break;

        /* Now a MIDI command. Check for running status (first byte not a status byte) */
        uint8_t first_byte = buf[cmd_off];
        if (first_byte >= 0xF8) {
            midi_net_emit_midi_message(first_byte, 0, 0);
            cmd_off++;
            continue;
        }
        if (first_byte & 0x80) {
            last_status = first_byte;
            cmd_off++;
        } else if (last_status == 0) {
            /* Malformed */
            midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
            break;
        }

        uint8_t type = last_status & 0xF0;
        int need = 0;
        switch (type) {
            case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: need = 2; break;
            case 0xC0: case 0xD0: need = 1; break;
            case 0xF0:
                if (last_status == 0xF7 && (!peer || peer->sysex_len == 0)) {
                    midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
                    last_status = 0;
                    continue;
                }
                if (last_status == 0xF0 ||
                    (last_status == 0xF7 && peer && peer->sysex_len > 0)) {
                    /* RFC 6295 SysEx command or segment. A first segment is
                     * F0 ... F0, a middle segment F7 ... F0, and the last
                     * segment F7 ... F7. Verbatim F0 ... F7 is the common
                     * single-packet form. */
                    uint8_t head = last_status;
                    int sx_start = cmd_off;
                    int sx_end = sx_start;
                    while (sx_end < cmd_end && !(buf[sx_end] & 0x80)) sx_end++;
                    int sx_bytes = sx_end - sx_start;

                    if (peer) {
                        uint8_t *pb = peer->sysex_buf;
                        if (head == 0xF0 && peer->sysex_len > 0) {
                            midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
                            peer->sysex_len = 0;
                            peer->sysex_overflow = 0;
                        }
                        if (head == 0xF0) {
                            if (peer->sysex_len < MIDI_NET_SYSEX_SCRATCH)
                                pb[peer->sysex_len++] = 0xF0;
                        }
                        for (int k = 0; k < sx_bytes; k++) {
                            if (peer->sysex_len < MIDI_NET_SYSEX_SCRATCH)
                                pb[peer->sysex_len++] = buf[sx_start + k];
                            else
                                peer->sysex_overflow = 1;
                        }
                        uint8_t tail = sx_end < cmd_end ? buf[sx_end] : 0;
                        if (tail == 0xF7) {
                            if (!peer->sysex_overflow &&
                                peer->sysex_len < MIDI_NET_SYSEX_SCRATCH) {
                                pb[peer->sysex_len++] = 0xF7;
                                midi_net_emit_sysex(pb, peer->sysex_len);
                            } else {
                                midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
                            }
                            peer->sysex_len = 0;
                            peer->sysex_overflow = 0;
                        } else if (tail == 0xF4) {
                            peer->sysex_len = 0;
                            peer->sysex_overflow = 0;
                        } else if (tail != 0xF0) {
                            midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
                            peer->sysex_len = 0;
                            peer->sysex_overflow = 0;
                        }
                    }
                    cmd_off = sx_end;
                    if (cmd_off < cmd_end) cmd_off++;
                    last_status = 0;
                    continue;
                }
                if (last_status == 0xF1 || last_status == 0xF3) need = 1;
                else if (last_status == 0xF2) need = 2;
                else need = 0;
                break;
        }

        if (cmd_off + need > cmd_end) {
            midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
            break;
        }
        uint8_t d1 = (need >= 1) ? buf[cmd_off] : 0;
        uint8_t d2 = (need >= 2) ? buf[cmd_off + 1] : 0;
        cmd_off += need;

        midi_net_emit_midi_message(last_status, d1, d2);
        if (last_status >= 0xF0) last_status = 0;
    }

    if (peer) {
        peer->last_status = last_status;
        peer->last_activity_ms = midi_net_now_ms();
    }

    (void)has_journal;  /* Journal is after command section — skipped. */
}

/* ============================================================================
 * Receive handlers (called from poll loop)
 * ============================================================================ */

void midi_net_applemidi_handle_ctrl(int sock) {
    uint8_t buf[2048];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    for (int loop = 0; loop < 16; loop++) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (n < 4) continue;

        midi_net_stat_inc_u64(&g_midi_net.stats.rx_packets);

        /* Control port only handles session commands */
        if (n >= 4 && rd16(buf) == APPLE_SIGNATURE) {
            handle_session_command(1, buf, (int)n, &from, fromlen);
        }
    }
}

void midi_net_applemidi_handle_evt(int sock) {
    uint8_t buf[2048];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    for (int loop = 0; loop < 16; loop++) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (n < 4) continue;

        midi_net_stat_inc_u64(&g_midi_net.stats.rx_packets);

        /* Event port can receive either session commands (IN/CK/BY)
         * or RTP-MIDI data packets. Detect by signature. */
        if (n >= 4 && rd16(buf) == APPLE_SIGNATURE) {
            handle_session_command(0, buf, (int)n, &from, fromlen);
        } else if (n >= 13 &&
                   (buf[0] & RTP_VERSION_MASK) == RTP_VERSION_2 &&
                   (buf[0] & 0x1F) == 0) {
            uint32_t ssrc = rd32(buf + 8);
            midi_net_peer_t *peer = find_peer_by_ssrc(ssrc);
            if (peer && same_ipv4_endpoint(&peer->evt_addr,
                                           peer->evt_addrlen, &from)) {
                parse_rtp_midi_packet(peer, buf, (int)n);
            } else {
                midi_net_stat_inc_u64(&g_midi_net.stats.drops_parse_err);
            }
        }
    }
}

/* ============================================================================
 * Periodic housekeeping — reap idle sessions
 * ============================================================================ */

void midi_net_applemidi_tick(void) {
    uint64_t now = midi_net_now_ms();
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        midi_net_peer_t *p = &g_midi_net.peers[i];
        if (!p->active) continue;
        if (now - p->last_activity_ms > 30000) {
            midi_net_log("AppleMIDI: reaping idle session ssrc=%08x", p->peer_ssrc);
            free_peer(p);
        }
    }
    update_peers_stat();
}

/* ============================================================================
 * Outbound — send a USB-MIDI packet to all established peers
 *
 * Builds a minimal RTP-MIDI packet (no journal) with a single command.
 * Called only by the network thread after it drains the lock-free queue.
 * ============================================================================ */

void midi_net_applemidi_send_midi(const uint8_t *pkt4) {
    if (!pkt4) return;
    uint8_t cin = pkt4[0] & 0x0F;
    uint8_t status = pkt4[1];
    uint8_t d1 = pkt4[2];
    uint8_t d2 = pkt4[3];

    /* Determine MIDI message length */
    int midi_len = 0;
    uint8_t midi_bytes[3];
    midi_bytes[0] = status;

    switch (cin) {
        case CIN_NOTE_OFF: case CIN_NOTE_ON: case CIN_POLY_KEY_PRESS:
        case CIN_CONTROL_CHANGE: case CIN_PITCH_BEND:
            midi_bytes[1] = d1; midi_bytes[2] = d2; midi_len = 3; break;
        case CIN_PROGRAM_CHANGE: case CIN_CHANNEL_PRESS:
            midi_bytes[1] = d1; midi_len = 2; break;
        case CIN_SYSTEM_COMMON_2:
            midi_bytes[1] = d1; midi_len = 2; break;
        case CIN_SYSTEM_COMMON_3:
            midi_bytes[1] = d1; midi_bytes[2] = d2; midi_len = 3; break;
        case CIN_SINGLE_BYTE:
            midi_len = 1; break;
        default:
            return;  /* Outbound SysEx is intentionally not implemented. */
    }

    /* Build RTP header + 1-byte command header + midi */
    int any_peers = 0;
    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        if (g_midi_net.peers[i].active >= 3) { any_peers = 1; break; }
    }
    if (!any_peers) return;

    uint8_t pkt[64];
    pkt[0] = 0x80;  /* V=2, P=0, X=0, CC=0 */
    pkt[1] = RTP_PAYLOAD_TYPE & 0x7F;  /* M=0 */
    wr16(pkt + 2, g_midi_net.applemidi_seq++);

    /* Timestamp: 100 µs ticks (latency bits = 1) */
    uint32_t ts = (uint32_t)midi_net_now_100us();
    wr32(pkt + 4, ts);

    /* Command section header: B=0, J=0, Z=0, P=0, length=midi_len */
    pkt[12] = (uint8_t)(midi_len & 0x0F);

    memcpy(pkt + 13, midi_bytes, midi_len);
    int total = 13 + midi_len;

    for (int i = 0; i < MIDI_NET_MAX_PEERS; i++) {
        midi_net_peer_t *p = &g_midi_net.peers[i];
        if (p->active < 3) continue;
        wr32(pkt + 8, p->ssrc);
        if (send_to_evt(p, pkt, total)) {
            midi_net_stat_inc_u64(&g_midi_net.stats.tx_packets);
        }
    }
}

#ifdef MIDI_NET_TESTING
void midi_net_test_parse_rtp(midi_net_peer_t *peer,
                             const uint8_t *packet, int len) {
    parse_rtp_midi_packet(peer, packet, len);
}
#endif
