/*
 * link_subscriber.cpp — Standalone Link Audio subscriber
 *
 * Hybrid approach:
 *   - Uses the Ableton Link SDK for session management and channel discovery
 *     (keeps the session alive via _asdp_v1 heartbeats)
 *   - Sends raw ChannelRequest packets ourselves to trigger Move's audio
 *     streaming (avoids SDK's LinkAudioSource which crashes on Move's kernel)
 *
 * The shim's sendto() hook intercepts the resulting audio packets.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ableton/LinkAudio.hpp>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_channels_changed{false};

/* Captured channel info for raw packet sending */
struct ChannelInfo {
    uint8_t id_bytes[8];
    char name[32];
};
static std::vector<ChannelInfo> g_move_channels;

/* Our subscriber peer ID (random, generated once) */
static uint8_t g_peer_id[8];

static void signal_handler(int sig)
{
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
        _exit(128 + sig);
    }
    g_running = false;
}

static bool is_link_audio_enabled()
{
    std::ifstream f("/data/UserData/move-anything/config/features.json");
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    auto pos = content.find("\"link_audio_enabled\"");
    if (pos == std::string::npos) return false;
    auto colon = content.find(':', pos);
    if (colon == std::string::npos) return false;
    auto nl = content.find('\n', colon);
    return content.find("true", colon) < nl;
}

static void generate_peer_id()
{
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        fread(g_peer_id, 1, sizeof(g_peer_id), f);
        fclose(f);
    }
}

/* Build a 28-byte ChannelRequest: 20-byte header + 8-byte raw ChannelId.
 * msg_type=4 (kChannelRequest), NOT TLV-wrapped. */
static void build_channel_request(uint8_t *pkt, const uint8_t *channel_id)
{
    memset(pkt, 0, 28);
    memcpy(pkt, "chnnlsv", 7);    /* magic */
    pkt[7] = 0x01;                  /* version */
    pkt[8] = 0x04;                  /* msg_type = kChannelRequest */
    /* pkt[9] = flags = 0, pkt[10..11] = reserved = 0 */
    memcpy(pkt + 12, g_peer_id, 8); /* subscriber PeerID */
    memcpy(pkt + 20, channel_id, 8); /* ChannelID to subscribe to */
}

int main()
{
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!is_link_audio_enabled()) {
        return 0;
    }

    generate_peer_id();
    printf("link-subscriber: starting\n");

    /* Phase 1: Join session and discover channels via SDK */
    ableton::LinkAudio link(120.0, "ME-Sub");
    link.enable(true);
    link.enableLinkAudio(true);

    link.setChannelsChangedCallback([&]() {
        auto chans = link.channels();

        g_move_channels.clear();
        for (const auto& ch : chans) {
            if (ch.peerName.find("Move") != std::string::npos) {
                ChannelInfo ci;
                /* Copy the 8-byte channel ID from the SDK's Id type */
                static_assert(sizeof(ch.id) >= 8, "ChannelId must be 8+ bytes");
                memcpy(ci.id_bytes, &ch.id, 8);
                snprintf(ci.name, sizeof(ci.name), "%s", ch.name.c_str());
                g_move_channels.push_back(ci);
            }
        }
        g_channels_changed = true;

        printf("link-subscriber: discovered %zu Move channels\n",
               g_move_channels.size());
        for (const auto& ci : g_move_channels) {
            printf("  %s\n", ci.name);
        }
    });

    /* Phase 2: Wait for channel discovery, then get Move's endpoint
     * from the shim (written to shared file when sendto captures it). */
    printf("link-subscriber: waiting for channel discovery...\n");

    /* The shim writes Move's chnnlsv endpoint to a file when it
     * captures the first sendto. We poll for it. If it doesn't exist,
     * Move isn't streaming yet (needs Live or another subscriber first).
     *
     * But we ARE the subscriber! Chicken-and-egg.
     *
     * Solution: use callOnLinkThread to send our ChannelRequests through
     * the SDK's own networking layer, bypassing the broken Source receiver. */

    /* Phase 2b: Send ChannelRequests via the SDK's network thread.
     * The SDK's UdpMessenger knows Move's endpoint. We send raw packets
     * by creating a separate socket and sending to the address the SDK
     * knows. We discover it from the _asdp_v1 data by listening to
     * multicast discovery ourselves.
     *
     * Actually, simplest: just open a raw socket, receive Move's
     * chnnlsv messages (session announcements), and get its address
     * from the source. Move sends session announcements to all known
     * peers — and since our SDK instance is a peer, Move sends to us. */

    /* Open a UDP socket to receive chnnlsv announcements and send requests */
    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("link-subscriber: socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* We need Move's chnnlsv address. The simplest reliable way:
     * listen on the same port the SDK bound to, receive Move's
     * session announcements, and extract the source address. */

    /* Actually, we'll take a simpler approach: just send ChannelRequests
     * to Move's link-local address on the port the shim captures.
     * The shim writes this to /tmp/link-audio-endpoint when it sees
     * the first chnnlsv packet from Move. */

    struct sockaddr_in6 move_addr;
    memset(&move_addr, 0, sizeof(move_addr));
    bool have_endpoint = false;

    while (g_running) {
        /* Try to read Move's endpoint from shim */
        if (!have_endpoint) {
            FILE *ep = fopen("/data/UserData/move-anything/link-audio-endpoint", "r");
            if (ep) {
                char addr_str[128];
                int port = 0;
                unsigned scope = 0;
                if (fscanf(ep, "%127s %d %u", addr_str, &port, &scope) == 3) {
                    move_addr.sin6_family = AF_INET6;
                    move_addr.sin6_port = htons(port);
                    move_addr.sin6_scope_id = scope;
                    if (inet_pton(AF_INET6, addr_str, &move_addr.sin6_addr) == 1) {
                        have_endpoint = true;
                        printf("link-subscriber: got Move endpoint %s port %d scope %u\n",
                               addr_str, port, scope);
                    }
                }
                fclose(ep);
            }
        }

        /* Send ChannelRequests for all discovered Move channels */
        if (have_endpoint && !g_move_channels.empty()) {
            for (const auto& ci : g_move_channels) {
                uint8_t pkt[28];
                build_channel_request(pkt, ci.id_bytes);
                sendto(sock, pkt, 28, 0,
                       (struct sockaddr *)&move_addr, sizeof(move_addr));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    close(sock);
    printf("link-subscriber: shutting down\n");
    return 0;
}
