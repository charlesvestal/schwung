/*
 * midi_net_mdns.c - Minimal mDNS/Bonjour responder for AppleMIDI session discovery
 *
 * Advertises a single _apple-midi._udp.local service pointing at
 * <hostname>.local:5004 so that macOS Audio MIDI Setup (and rtpMIDI on
 * Windows) can auto-discover this Move as a Network MIDI endpoint.
 *
 * Scope is deliberately minimal:
 *   - Answers PTR queries for _apple-midi._udp.local
 *   - Answers SRV queries for our instance
 *   - Answers A queries for <hostname>.local
 *   - Unsolicited announcement on startup and every 120s
 *   - No browse, no conflict detection, no TSIG, no goodbye packets
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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

#define MDNS_DNS_CLASS_IN    1
#define MDNS_FLAG_CACHE_FLUSH 0x8000
#define MDNS_RR_A            1
#define MDNS_RR_PTR          12
#define MDNS_RR_TXT          16
#define MDNS_RR_SRV          33
#define MDNS_RR_ANY          255
#define MDNS_TTL             120

static char g_hostname[64] = "schwung";
static char g_instance_label[96] = MIDI_NET_SESSION_NAME;

/* ============================================================================
 * DNS name encoding helpers
 * ============================================================================ */

/* Encode "foo.bar.baz" as DNS labels: \x03foo\x03bar\x03baz\x00
 * Returns number of bytes written (including trailing 0). */
static int encode_dns_name(uint8_t *out, int max_len, const char *name) {
    int out_i = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int label_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (label_len == 0) { p++; continue; }
        if (label_len > 63) label_len = 63;
        if (out_i + 1 + label_len + 1 > max_len) return -1;
        out[out_i++] = (uint8_t)label_len;
        memcpy(out + out_i, p, label_len);
        out_i += label_len;
        p += label_len;
        if (*p == '.') p++;
    }
    if (out_i + 1 > max_len) return -1;
    out[out_i++] = 0;
    return out_i;
}

/* Decode DNS name from packet (with pointer compression support).
 * Writes dotted string to out. Returns number of bytes consumed in the
 * query section (which may differ from the expanded name length if
 * pointers are used). */
static int decode_dns_name(const uint8_t *pkt, int pkt_len, int offset,
                           char *out, int out_max) {
    int out_i = 0;
    int consumed = 0;
    int visited_ptr = 0;
    int pointer_jumps = 0;
    int i = offset;

    if (!out || out_max <= 0 || offset < 0 || offset >= pkt_len) return -1;

    while (i < pkt_len) {
        uint8_t b = pkt[i];
        if (b == 0) {
            i++;
            if (!visited_ptr) consumed = i - offset;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            if (i + 1 >= pkt_len) return -1;
            int ptr = ((b & 0x3F) << 8) | pkt[i + 1];
            if (ptr >= pkt_len || ++pointer_jumps > 16) return -1;
            if (!visited_ptr) {
                consumed = (i + 2) - offset;
                visited_ptr = 1;
            }
            i = ptr;
            continue;
        }
        if ((b & 0xC0) != 0) return -1;
        int llen = b;
        i++;
        if (i + llen > pkt_len) return -1;
        if (out_i + llen + 1 >= out_max) return -1;
        if (out_i > 0) out[out_i++] = '.';
        memcpy(out + out_i, pkt + i, llen);
        out_i += llen;
        i += llen;
    }
    out[out_i] = 0;
    if (!visited_ptr && consumed == 0) consumed = i - offset;
    return consumed;
}

/* ============================================================================
 * Hostname / instance helpers
 * ============================================================================ */

static void sanitize_hostname(char *s) {
    for (char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-')) *p = '-';
    }
}

static void init_names_if_needed(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;

    char host[64];
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = 0;
        /* Strip any domain suffix — mDNS uses unqualified hostnames */
        char *dot = strchr(host, '.');
        if (dot) *dot = 0;
        sanitize_hostname(host);
        if (host[0]) {
            strncpy(g_hostname, host, sizeof(g_hostname) - 1);
            g_hostname[sizeof(g_hostname) - 1] = 0;

            /* Build instance label: "<SessionName> (<hostname>)" */
            snprintf(g_instance_label, sizeof(g_instance_label),
                     "%s (%s)", MIDI_NET_SESSION_NAME, host);
        }
    }
}

/* ============================================================================
 * Socket lifecycle
 * ============================================================================ */

int midi_net_mdns_open(void) {
    init_names_if_needed();

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        midi_net_log("mDNS: socket failed: %s", strerror(errno));
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
    addr.sin_port = htons(MIDI_NET_MDNS_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        midi_net_log("mDNS: bind :%d failed: %s (avahi running?)",
                     MIDI_NET_MDNS_PORT, strerror(errno));
        close(sock);
        return -1;
    }

    /* Join 224.0.0.251 on all interfaces */
    struct in_addr group;
    inet_pton(AF_INET, MIDI_NET_MDNS_GROUP, &group);

    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) == 0) {
        for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            if (!(ifa->ifa_flags & IFF_MULTICAST)) continue;

            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            struct ip_mreq mreq;
            memset(&mreq, 0, sizeof(mreq));
            mreq.imr_multiaddr = group;
            mreq.imr_interface = sin->sin_addr;
            setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }
        freeifaddrs(ifa_list);
    }

    /* TTL 255 per RFC 6762 */
    uint8_t ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    g_midi_net.mdns_sock = sock;
    return 0;
}

void midi_net_mdns_close(void) {
    if (g_midi_net.mdns_sock >= 0) {
        close(g_midi_net.mdns_sock);
        g_midi_net.mdns_sock = -1;
    }
}

/* ============================================================================
 * Get our IPv4 for a given interface (for A record answers)
 * ============================================================================ */

static uint32_t get_preferred_ipv4(void) {
    struct ifaddrs *ifa_list = NULL;
    uint32_t best = 0;
    if (getifaddrs(&ifa_list) != 0) return 0;

    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t a = sin->sin_addr.s_addr;

        /* Prefer wlan0 if present */
        if (strncmp(ifa->ifa_name, "wlan", 4) == 0) {
            best = a;
            break;
        }
        if (best == 0) best = a;
    }
    freeifaddrs(ifa_list);
    return best;
}

/* ============================================================================
 * Build full DNS response with PTR + SRV + TXT + A records
 * ============================================================================ */

static int build_response(uint8_t *out, int max_len, uint16_t query_id,
                          int include_ptr, int include_srv, int include_txt,
                          int include_a) {
    if (max_len < 12) return 0;

    uint32_t preferred_ipv4 = include_a ? get_preferred_ipv4() : 0;
    if (!preferred_ipv4) include_a = 0;
    int nanswers = (include_ptr ? 1 : 0) + (include_srv ? 1 : 0) +
                   (include_txt ? 1 : 0) + (include_a ? 1 : 0);
    if (nanswers == 0) return 0;

    /* DNS header */
    /* Multicast DNS messages use transaction ID zero (RFC 6762 section 18). */
    (void)query_id;
    out[0] = 0;
    out[1] = 0;
    out[2] = 0x84;  /* Response, Authoritative */
    out[3] = 0x00;
    out[4] = 0x00; out[5] = 0x00;  /* QDCOUNT */
    out[6] = 0x00; out[7] = (uint8_t)nanswers;  /* ANCOUNT */
    out[8] = 0x00; out[9] = 0x00;  /* NSCOUNT */
    out[10] = 0x00; out[11] = 0x00;  /* ARCOUNT */
    int off = 12;

    /* Build service name: "<instance>._apple-midi._udp.local" */
    char service_full[192];
    snprintf(service_full, sizeof(service_full), "%s._apple-midi._udp.local",
             g_instance_label);

    const char *service_type = "_apple-midi._udp.local";

    /* Build host name: "<hostname>.local" */
    char hostname_full[96];
    snprintf(hostname_full, sizeof(hostname_full), "%s.local", g_hostname);

    /* --- PTR: _apple-midi._udp.local -> <instance>._apple-midi._udp.local --- */
    if (include_ptr) {
        int n = encode_dns_name(out + off, max_len - off, service_type);
        if (n < 0) return 0;
        off += n;
        if (off + 10 > max_len) return 0;
        out[off++] = 0; out[off++] = MDNS_RR_PTR;
        out[off++] = 0; out[off++] = MDNS_DNS_CLASS_IN;
        out[off++] = 0; out[off++] = 0; out[off++] = 0; out[off++] = MDNS_TTL;
        int rdlen_off = off;
        out[off++] = 0; out[off++] = 0;
        int rd_start = off;
        int nn = encode_dns_name(out + off, max_len - off, service_full);
        if (nn < 0) return 0;
        off += nn;
        int rdlen = off - rd_start;
        out[rdlen_off] = (rdlen >> 8) & 0xFF;
        out[rdlen_off + 1] = rdlen & 0xFF;
    }

    /* --- SRV: <instance> -> <hostname>.local:5004 --- */
    if (include_srv) {
        int n = encode_dns_name(out + off, max_len - off, service_full);
        if (n < 0) return 0;
        off += n;
        if (off + 16 > max_len) return 0;
        out[off++] = 0; out[off++] = MDNS_RR_SRV;
        out[off++] = (MDNS_FLAG_CACHE_FLUSH >> 8) & 0xFF;
        out[off++] = MDNS_DNS_CLASS_IN;
        out[off++] = 0; out[off++] = 0; out[off++] = 0; out[off++] = MDNS_TTL;
        int rdlen_off = off;
        out[off++] = 0; out[off++] = 0;
        int rd_start = off;
        out[off++] = 0; out[off++] = 0;           /* Priority */
        out[off++] = 0; out[off++] = 0;           /* Weight */
        out[off++] = (MIDI_NET_APPLE_CTRL_PORT >> 8) & 0xFF;
        out[off++] = MIDI_NET_APPLE_CTRL_PORT & 0xFF;
        int nn = encode_dns_name(out + off, max_len - off, hostname_full);
        if (nn < 0) return 0;
        off += nn;
        int rdlen = off - rd_start;
        out[rdlen_off] = (rdlen >> 8) & 0xFF;
        out[rdlen_off + 1] = rdlen & 0xFF;
    }

    /* --- TXT: empty --- */
    if (include_txt) {
        int n = encode_dns_name(out + off, max_len - off, service_full);
        if (n < 0) return 0;
        off += n;
        if (off + 11 > max_len) return 0;
        out[off++] = 0; out[off++] = MDNS_RR_TXT;
        out[off++] = (MDNS_FLAG_CACHE_FLUSH >> 8) & 0xFF;
        out[off++] = MDNS_DNS_CLASS_IN;
        out[off++] = 0; out[off++] = 0; out[off++] = 0; out[off++] = MDNS_TTL;
        out[off++] = 0; out[off++] = 1;  /* rdlen = 1 */
        out[off++] = 0;                  /* empty TXT (single zero-length string) */
    }

    /* --- A: <hostname>.local -> our IPv4 --- */
    if (include_a) {
        int n = encode_dns_name(out + off, max_len - off, hostname_full);
        if (n < 0) return 0;
        off += n;
        if (off + 14 > max_len) return 0;
        out[off++] = 0; out[off++] = MDNS_RR_A;
        out[off++] = (MDNS_FLAG_CACHE_FLUSH >> 8) & 0xFF;
        out[off++] = MDNS_DNS_CLASS_IN;
        out[off++] = 0; out[off++] = 0; out[off++] = 0; out[off++] = MDNS_TTL;
        out[off++] = 0; out[off++] = 4;  /* rdlen = 4 */
        memcpy(out + off, &preferred_ipv4, 4);
        off += 4;
    }

    return off;
}

/* ============================================================================
 * Announcement & query handling
 * ============================================================================ */

void midi_net_mdns_announce(void) {
    if (g_midi_net.mdns_sock < 0) return;
    init_names_if_needed();

    uint8_t pkt[512];
    int len = build_response(pkt, sizeof(pkt), 0, 1, 1, 1, 1);
    if (len <= 0) return;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, MIDI_NET_MDNS_GROUP, &dst.sin_addr);
    dst.sin_port = htons(MIDI_NET_MDNS_PORT);

    if (sendto(g_midi_net.mdns_sock, pkt, len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        midi_net_logd("mDNS: announce send failed: %s", strerror(errno));
    } else {
        __atomic_fetch_add(&g_midi_net.stats.mdns_responses, 1u,
                           __ATOMIC_RELAXED);
        midi_net_logd("mDNS: sent unsolicited announce (%d bytes)", len);
    }
}

void midi_net_mdns_handle_rx(int sock) {
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
            break;
        }
        if (n < 12) continue;

        /* DNS header */
        uint16_t query_id = (buf[0] << 8) | buf[1];
        uint16_t flags = (buf[2] << 8) | buf[3];
        uint16_t qdcount = (buf[4] << 8) | buf[5];

        /* Ignore responses (only care about queries) */
        if (flags & 0x8000) continue;
        if (qdcount == 0) continue;

        __atomic_fetch_add(&g_midi_net.stats.mdns_queries, 1u,
                           __ATOMIC_RELAXED);

        int off = 12;
        int answer_ptr = 0, answer_srv = 0, answer_txt = 0, answer_a = 0;

        for (int q = 0; q < qdcount && off < (int)n; q++) {
            char qname[256];
            int consumed = decode_dns_name(buf, (int)n, off, qname, sizeof(qname));
            if (consumed < 0) break;
            off += consumed;
            if (off + 4 > (int)n) break;
            uint16_t qtype = (buf[off] << 8) | buf[off + 1];
            off += 4;  /* skip qtype + qclass */

            if (strcasecmp(qname, "_apple-midi._udp.local") == 0) {
                if (qtype == MDNS_RR_PTR || qtype == MDNS_RR_ANY) {
                    answer_ptr = 1;
                    answer_srv = 1;
                    answer_txt = 1;
                    answer_a = 1;
                }
            } else {
                /* Check if it matches our instance or hostname */
                char service_full[192];
                snprintf(service_full, sizeof(service_full),
                         "%s._apple-midi._udp.local", g_instance_label);
                char hostname_full[96];
                snprintf(hostname_full, sizeof(hostname_full), "%s.local", g_hostname);

                if (strcasecmp(qname, service_full) == 0) {
                    if (qtype == MDNS_RR_SRV || qtype == MDNS_RR_ANY) answer_srv = 1;
                    if (qtype == MDNS_RR_TXT || qtype == MDNS_RR_ANY) answer_txt = 1;
                    if (qtype == MDNS_RR_ANY) { answer_ptr = 1; answer_a = 1; }
                } else if (strcasecmp(qname, hostname_full) == 0) {
                    if (qtype == MDNS_RR_A || qtype == MDNS_RR_ANY) answer_a = 1;
                }
            }
        }

        if (answer_ptr || answer_srv || answer_txt || answer_a) {
            uint8_t pkt[512];
            int len = build_response(pkt, sizeof(pkt), query_id,
                                     answer_ptr, answer_srv, answer_txt, answer_a);
            if (len > 0) {
                struct sockaddr_in dst;
                memset(&dst, 0, sizeof(dst));
                dst.sin_family = AF_INET;
                inet_pton(AF_INET, MIDI_NET_MDNS_GROUP, &dst.sin_addr);
                dst.sin_port = htons(MIDI_NET_MDNS_PORT);
                if (sendto(sock, pkt, len, 0,
                           (struct sockaddr *)&dst, sizeof(dst)) == len) {
                    __atomic_fetch_add(&g_midi_net.stats.mdns_responses, 1u,
                                       __ATOMIC_RELAXED);
                }
            }
        }
    }
}

#ifdef MIDI_NET_TESTING
int midi_net_test_decode_dns_name(const uint8_t *packet, int packet_len,
                                  int offset, char *out, int out_len) {
    return decode_dns_name(packet, packet_len, offset, out, out_len);
}
#endif
