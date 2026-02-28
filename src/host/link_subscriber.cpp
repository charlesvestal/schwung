/*
 * link_subscriber.cpp — Standalone Link Audio subscriber
 *
 * Uses the Ableton Link SDK's LinkAudioSource to subscribe to Move's
 * per-track audio channels. This triggers Move to stream audio via
 * chnnlsv, which the shim's sendto() hook intercepts.
 *
 * Running as a standalone process (not inside Move's LD_PRELOAD shim)
 * avoids the hook conflicts that caused SIGSEGV in the in-shim approach.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ableton/LinkAudio.hpp>

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "unified_log.h"

static std::atomic<bool> g_running{true};

#define LINK_SUB_LOG_SOURCE "link_subscriber"

/* Channel IDs discovered via callback — processed in main loop */
struct PendingChannel {
    ableton::ChannelId id;
    std::string peerName;
    std::string name;
};
static std::mutex g_pending_mu;
static std::vector<PendingChannel> g_pending_channels;
static std::atomic<bool> g_channels_changed{false};

static std::atomic<uint64_t> g_buffers_received{0};

static void signal_handler(int sig)
{
    /* Use write() — async-signal-safe, unlike printf() */
    const char *msg = "link-subscriber: caught signal\n";
    switch (sig) {
        case SIGSEGV: msg = "link-subscriber: SIGSEGV\n"; break;
        case SIGBUS:  msg = "link-subscriber: SIGBUS\n"; break;
        case SIGABRT: msg = "link-subscriber: SIGABRT\n"; break;
        case SIGTERM: msg = "link-subscriber: SIGTERM\n"; break;
        case SIGINT:  msg = "link-subscriber: SIGINT\n"; break;
    }
    (void)write(STDOUT_FILENO, msg, strlen(msg));

    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
        /* _exit() skips destructors — acceptable for fatal signals */
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

int main()
{
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGBUS, signal_handler);
    std::signal(SIGABRT, signal_handler);

    unified_log_init();

    if (!is_link_audio_enabled()) {
        unified_log_shutdown();
        return 0;
    }

    LOG_INFO(LINK_SUB_LOG_SOURCE, "starting");

    /* Read initial tempo from file written by shim (falls back to 120) */
    double initial_tempo = 120.0;
    {
        FILE *fp = fopen("/tmp/link-tempo", "r");
        if (fp) {
            double t = 0.0;
            if (fscanf(fp, "%lf", &t) == 1 && t >= 20.0 && t <= 999.0) {
                initial_tempo = t;
                printf("link-subscriber: using set tempo %.1f BPM\n", t);
            }
            fclose(fp);
        }
        if (initial_tempo == 120.0) {
            printf("link-subscriber: using default tempo 120.0 BPM\n");
        }
    }

    /* Join Link session and enable audio */
    ableton::LinkAudio link(initial_tempo, "ME-Sub");
    link.enable(true);
    link.enableLinkAudio(true);

    /* Create a dummy sink so that our PeerAnnouncements include at least one
     * channel.  Move's Sink handler looks up ChannelRequest.peerId in
     * mPeerSendHandlers, which is only populated when a PeerAnnouncement
     * with channels is received.  Without this, forPeer() returns nullopt
     * and audio is silently never sent. */
    ableton::LinkAudioSink dummySink(link, "ME-Sub-Ack", 256);
    LOG_INFO(LINK_SUB_LOG_SOURCE, "dummy sink created (triggers peer announcement)");

    /* Callback just records channel IDs — source creation deferred to main loop
     * because LinkAudioSource constructor isn't safe from the callback thread */
    link.setChannelsChangedCallback([&]() {
        auto channels = link.channels();
        std::lock_guard<std::mutex> lock(g_pending_mu);
        g_pending_channels.clear();
        for (const auto& ch : channels) {
            if (ch.peerName.find("Move") != std::string::npos) {
                g_pending_channels.push_back({ch.id, ch.peerName, ch.name});
            }
        }
        g_channels_changed = true;
        LOG_INFO(LINK_SUB_LOG_SOURCE, "discovered %zu Move channels",
                 g_pending_channels.size());
    });

    LOG_INFO(LINK_SUB_LOG_SOURCE, "waiting for channel discovery...");

    /* Active sources — managed in main loop only */
    std::vector<ableton::LinkAudioSource> sources;

    uint64_t last_count = 0;
    int tick = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        tick++;

        /* Create sources when channels change */
        if (g_channels_changed.exchange(false)) {
            std::vector<PendingChannel> pending;
            {
                std::lock_guard<std::mutex> lock(g_pending_mu);
                pending = g_pending_channels;
            }

            /* Destroy old sources first */
            sources.clear();
            LOG_INFO(LINK_SUB_LOG_SOURCE, "cleared old sources");

            /* Small delay to let SDK process the unsubscriptions */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            /* Pre-allocate to avoid reallocation (which moves LinkAudioSource objects) */
            sources.reserve(pending.size());

            /* Create new sources one at a time */
            for (const auto& pc : pending) {
                LOG_INFO(LINK_SUB_LOG_SOURCE, "subscribing to %s/%s...",
                         pc.peerName.c_str(), pc.name.c_str());

                try {
                    sources.emplace_back(link, pc.id,
                        [](ableton::LinkAudioSource::BufferHandle) {
                            g_buffers_received.fetch_add(1, std::memory_order_relaxed);
                        });
                    LOG_INFO(LINK_SUB_LOG_SOURCE, "subscription OK");
                } catch (const std::exception& e) {
                    LOG_ERROR(LINK_SUB_LOG_SOURCE, "subscription failed: %s", e.what());
                } catch (...) {
                    LOG_ERROR(LINK_SUB_LOG_SOURCE, "subscription failed: unknown error");
                }

                /* Small delay between source creation to avoid overwhelming */
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            LOG_INFO(LINK_SUB_LOG_SOURCE, "%zu sources active", sources.size());
        }

        /* Log buffer count every 30 seconds */
        if (tick % 60 == 0) {
            uint64_t count = g_buffers_received.load(std::memory_order_relaxed);
            if (count != last_count) {
                LOG_INFO(LINK_SUB_LOG_SOURCE, "%llu audio buffers received",
                         (unsigned long long)count);
                last_count = count;
            }
        }
    }

    sources.clear();
    LOG_INFO(LINK_SUB_LOG_SOURCE, "shutting down (%llu total buffers)",
             (unsigned long long)g_buffers_received.load());
    unified_log_shutdown();
    return 0;
}
