/*
 * midi_net_ipmidi.c - ipMIDI multicast backend
 *
 * Receives raw MIDI over UDP multicast (group 225.0.0.37, port 21928).
 * ipMIDI is a stateless, session-less protocol used by iConnectivity's
 * mioXL, MidiOverLan CP, Tobias Erichsen's rtpMIDI in "ipMIDI mode", and
 * the `sendmidi` CLI tool. Payload is simply concatenated raw MIDI bytes
 * with running status preserved across messages.
 *
 * Running status and partial messages are tracked independently per sender.
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
#include <ifaddrs.h>
#include <net/if.h>

#include "midi_net.h"
#include "midi_net_internal.h"

/* ============================================================================
 * Multicast group join across all interfaces
 * ============================================================================ */

static int join_on_all_interfaces(int sock, const char *group_addr) {
    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0) {
        midi_net_log("ipMIDI: getifaddrs failed: %s", strerror(errno));
        return -1;
    }

    int joined = 0;
    struct in_addr group;
    inet_pton(AF_INET, group_addr, &group);

    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;
        if (!(ifa->ifa_flags & IFF_MULTICAST) && !(ifa->ifa_flags & IFF_LOOPBACK)) continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;

        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = group;
        mreq.imr_interface = sin->sin_addr;

        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) == 0) {
            joined++;
            char ipbuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf));
            midi_net_logd("ipMIDI: joined %s on %s (%s)",
                          group_addr, ifa->ifa_name, ipbuf);
        } else if (errno != EADDRINUSE) {
            midi_net_logd("ipMIDI: join on %s failed: %s",
                          ifa->ifa_name, strerror(errno));
        }
    }
    freeifaddrs(ifa_list);
    return joined;
}

/* ============================================================================
 * Socket lifecycle
 * ============================================================================ */

int midi_net_ipmidi_open(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        midi_net_log("ipMIDI: socket failed: %s", strerror(errno));
        return -1;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MIDI_NET_IPMIDI_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        midi_net_log("ipMIDI: bind to :%d failed: %s",
                     MIDI_NET_IPMIDI_PORT, strerror(errno));
        close(sock);
        return -1;
    }

    /* Keep loopback enabled: this backend is inbound-only and local test tools
     * are useful during development. */
    uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    int joined = join_on_all_interfaces(sock, MIDI_NET_IPMIDI_GROUP);
    if (joined <= 0) {
        midi_net_log("ipMIDI: no multicast-capable interfaces are currently up");
    }

    g_midi_net.ipmidi_sock = sock;
    memset(g_midi_net.ipmidi_sources, 0, sizeof(g_midi_net.ipmidi_sources));
    return 0;
}

static int same_sender(const midi_net_ipmidi_source_t *source,
                       const struct sockaddr_in *from) {
    if (!source->active || source->addr.ss_family != AF_INET) return 0;
    const struct sockaddr_in *known =
        (const struct sockaddr_in *)&source->addr;
    return known->sin_addr.s_addr == from->sin_addr.s_addr &&
           known->sin_port == from->sin_port;
}

static midi_net_ipmidi_source_t *parser_for_sender(
        const struct sockaddr_in *from, socklen_t fromlen) {
    uint64_t oldest_ms = UINT64_MAX;
    midi_net_ipmidi_source_t *oldest = NULL;
    for (int i = 0; i < MIDI_NET_MAX_IPMIDI_SOURCES; i++) {
        midi_net_ipmidi_source_t *source = &g_midi_net.ipmidi_sources[i];
        if (same_sender(source, from)) return source;
        if (!source->active) { oldest = source; break; }
        if (source->last_activity_ms < oldest_ms) {
            oldest_ms = source->last_activity_ms;
            oldest = source;
        }
    }
    if (!oldest) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->active = 1;
    memcpy(&oldest->addr, from, fromlen);
    oldest->addrlen = fromlen;
    return oldest;
}

void midi_net_ipmidi_close(void) {
    if (g_midi_net.ipmidi_sock >= 0) {
        close(g_midi_net.ipmidi_sock);
        g_midi_net.ipmidi_sock = -1;
    }
}

/* ============================================================================
 * Receive and parse
 * ============================================================================ */

void midi_net_ipmidi_handle_rx(int sock) {
    uint8_t buf[1500];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    for (int loop = 0; loop < 16; loop++) {
        fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            midi_net_logd("ipMIDI: recvfrom error: %s", strerror(errno));
            break;
        }
        if (n == 0) continue;

        midi_net_stat_inc_u64(&g_midi_net.stats.rx_packets);

        midi_net_ipmidi_source_t *source =
            parser_for_sender(&from, fromlen);
        if (!source) continue;
        source->last_activity_ms = midi_net_now_ms();
        int msgs = midi_net_parse_raw_stream(&source->parser, buf, (int)n);
        uint64_t rx = __atomic_load_n(&g_midi_net.stats.rx_packets,
                                      __ATOMIC_RELAXED);
        if (rx <= 5 || (rx % 1024) == 0) {
            char src[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
            midi_net_logd("ipMIDI: rx %zd bytes from %s, parsed %d msgs", n, src, msgs);
        }
    }
}
