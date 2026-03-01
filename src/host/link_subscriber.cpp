/*
 * link_subscriber.cpp — Link Audio subscriber + publisher bridge
 *
 * Subscriber side:
 *   Uses the Ableton Link SDK's LinkAudioSource to subscribe to Move's
 *   per-track audio channels. This triggers Move to stream audio via
 *   chnnlsv, which the shim's sendto() hook intercepts.
 *
 * Publisher side:
 *   Reads per-slot shadow audio from shared memory (written by the shim)
 *   and publishes it to the Link session via LinkAudioSink. This makes
 *   shadow slot audio visible to Live as Link Audio channels.
 *
 * Running as a standalone process (not inside Move's LD_PRELOAD shim)
 * avoids the hook conflicts that caused SIGSEGV in the in-shim approach.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <ableton/LinkAudio.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Shared memory layout for publisher audio */
extern "C" {
#include "link_audio.h"
}

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
static std::atomic<uint64_t> g_buffers_published{0};

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

/* Open the publisher shared memory segment (created by shim) */
static link_audio_pub_shm_t *open_pub_shm()
{
    int fd = shm_open(SHM_LINK_AUDIO_PUB, O_RDWR, 0666);
    if (fd < 0) return nullptr;

    auto *shm = (link_audio_pub_shm_t *)mmap(nullptr,
        sizeof(link_audio_pub_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm == MAP_FAILED) return nullptr;
    if (shm->magic != LINK_AUDIO_PUB_SHM_MAGIC) {
        munmap(shm, sizeof(link_audio_pub_shm_t));
        return nullptr;
    }

    return shm;
}

/* Per-slot publisher state */
struct SlotPublisher {
    ableton::LinkAudioSink *sink = nullptr;
    uint32_t last_read_pos = 0;
    bool was_active = false;
};

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
    ableton::LinkAudio link(initial_tempo, "ME");
    link.enable(true);
    link.enableLinkAudio(true);

    printf("link-subscriber: Link session joined\n");

    /* Create a dummy sink so that our PeerAnnouncements include at least one
     * channel.  Move's Sink handler looks up ChannelRequest.peerId in
     * mPeerSendHandlers, which is only populated when a PeerAnnouncement
     * with channels is received.  Without this, forPeer() returns nullopt
     * and audio is silently never sent. */
    ableton::LinkAudioSink dummySink(link, "ME-Ack", 256);
    LOG_INFO(LINK_SUB_LOG_SOURCE, "dummy sink created (triggers peer announcement)");

    /* Publisher sinks for shadow slots (4 per-track + 1 master) */
    SlotPublisher slots[LINK_AUDIO_PUB_SLOT_COUNT];

    /* Callback records channel IDs — source creation deferred to main loop */
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

    /* Try to open publisher shared memory */
    link_audio_pub_shm_t *pub_shm = nullptr;
    int pub_shm_retries = 0;

    uint64_t last_rx_count = 0;
    uint64_t last_tx_count = 0;
    int tick = 0;

    while (g_running) {
        /* Use a shorter sleep to poll the publisher shm more frequently.
         * The shim writes at ~344 Hz (every ~2.9ms). We poll at ~100 Hz
         * which means ~3 render blocks accumulate between polls.
         * The SDK handles the 128→125 repacketing internally. */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        tick++;

        /* Create sources when channels change (every ~500ms worth of ticks) */
        if (g_channels_changed.exchange(false)) {
            std::vector<PendingChannel> pending;
            {
                std::lock_guard<std::mutex> lock(g_pending_mu);
                pending = g_pending_channels;
            }

            /* Skip if no Move channels found — our own sink creation triggers
             * this callback, and Move's channels may not be in the transient list.
             * Clearing sources here would kill audio flow and trigger stale restart. */
            if (pending.empty()) {
                LOG_INFO(LINK_SUB_LOG_SOURCE, "channels changed but no Move channels found, keeping existing sources");
                continue;
            }

            sources.clear();
            LOG_INFO(LINK_SUB_LOG_SOURCE, "cleared old sources");

            /* Small delay to let SDK process the unsubscriptions */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            sources.reserve(pending.size());

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
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            LOG_INFO(LINK_SUB_LOG_SOURCE, "%zu sources active", sources.size());
        }

        /* Try to open publisher shm if not yet available */
        if (!pub_shm && pub_shm_retries < 600) {
            /* Retry every ~1 second (100 ticks * 10ms) */
            if (tick % 100 == 0) {
                pub_shm = open_pub_shm();
                pub_shm_retries++;
                if (pub_shm) {
                    LOG_INFO(LINK_SUB_LOG_SOURCE, "publisher shm opened");
                    /* Sync read positions to current write positions */
                    for (int i = 0; i < LINK_AUDIO_PUB_SLOT_COUNT; i++) {
                        slots[i].last_read_pos = pub_shm->slots[i].write_pos;
                    }
                }
            }
        }

        /* --- Publisher: read from shm, write to sinks --- */
        if (pub_shm) {
            for (int i = 0; i < LINK_AUDIO_PUB_SLOT_COUNT; i++) {
                link_audio_pub_slot_t *ps = &pub_shm->slots[i];
                bool is_active = ps->active != 0;

                /* Create/destroy sinks as slots activate/deactivate */
                if (is_active && !slots[i].was_active) {
                    char name[32];
                    if (i == LINK_AUDIO_PUB_MASTER_IDX)
                        snprintf(name, sizeof(name), "ME-Master");
                    else
                        snprintf(name, sizeof(name), "ME-%d", i + 1);
                    try {
                        /* maxNumSamples: 128 frames * 2 channels = 256 samples */
                        slots[i].sink = new ableton::LinkAudioSink(link, name, 256);
                        LOG_INFO(LINK_SUB_LOG_SOURCE, "created sink %s", name);
                    } catch (...) {
                        LOG_ERROR(LINK_SUB_LOG_SOURCE, "failed to create sink %s", name);
                        slots[i].sink = nullptr;
                    }
                    slots[i].last_read_pos = ps->write_pos;
                    slots[i].was_active = true;
                } else if (!is_active && slots[i].was_active) {
                    if (slots[i].sink) {
                        delete slots[i].sink;
                        slots[i].sink = nullptr;
                        char name[32];
                        if (i == LINK_AUDIO_PUB_MASTER_IDX)
                            snprintf(name, sizeof(name), "ME-Master");
                        else
                            snprintf(name, sizeof(name), "ME-%d", i + 1);
                        LOG_INFO(LINK_SUB_LOG_SOURCE, "destroyed sink %s", name);
                    }
                    slots[i].was_active = false;
                }

                /* Publish audio if sink exists and data is available */
                if (!slots[i].sink || !is_active) continue;

                uint32_t wp = ps->write_pos;
                __sync_synchronize();
                uint32_t rp = slots[i].last_read_pos;
                uint32_t avail = wp - rp;

                /* Skip if no new data or if we've fallen too far behind */
                if (avail == 0) continue;
                if (avail > LINK_AUDIO_PUB_SHM_RING_SAMPLES) {
                    /* Overrun — reset to current write position */
                    slots[i].last_read_pos = wp;
                    continue;
                }

                /* Drain in 128-frame (256-sample) blocks */
                while (avail >= LINK_AUDIO_PUB_BLOCK_SAMPLES) {
                    auto buffer = ableton::LinkAudioSink::BufferHandle(*slots[i].sink);
                    if (buffer) {
                        /* Copy 128 stereo frames from ring to sink buffer */
                        for (int s = 0; s < LINK_AUDIO_PUB_BLOCK_SAMPLES; s++) {
                            buffer.samples[s] = ps->ring[rp & LINK_AUDIO_PUB_SHM_RING_MASK];
                            rp++;
                        }

                        auto sessionState = link.captureAudioSessionState();
                        auto hostTime = std::chrono::microseconds(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()
                            ).count()
                        );
                        double beats = sessionState.beatAtTime(hostTime, 4.0);

                        buffer.commit(sessionState,
                                      beats,
                                      4.0,                        /* quantum */
                                      LINK_AUDIO_PUB_BLOCK_FRAMES, /* numFrames */
                                      2,                           /* stereo */
                                      44100);                      /* sampleRate */

                        g_buffers_published.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        /* No subscriber for this sink — advance read pointer anyway */
                        rp += LINK_AUDIO_PUB_BLOCK_SAMPLES;
                    }

                    avail = wp - rp;
                }

                slots[i].last_read_pos = rp;
            }
        }

        /* Log stats every 30 seconds (3000 ticks at 10ms) */
        if (tick % 3000 == 0) {
            uint64_t rx = g_buffers_received.load(std::memory_order_relaxed);
            uint64_t tx = g_buffers_published.load(std::memory_order_relaxed);
            if (rx != last_rx_count || tx != last_tx_count) {
                LOG_INFO(LINK_SUB_LOG_SOURCE, "rx=%llu tx=%llu",
                         (unsigned long long)rx, (unsigned long long)tx);
                last_rx_count = rx;
                last_tx_count = tx;
            }
        }
    }

    /* Cleanup */
    sources.clear();
    for (int i = 0; i < LINK_AUDIO_PUB_SLOT_COUNT; i++) {
        if (slots[i].sink) {
            delete slots[i].sink;
            slots[i].sink = nullptr;
        }
    }

    if (pub_shm) {
        munmap(pub_shm, sizeof(link_audio_pub_shm_t));
    }

    LOG_INFO(LINK_SUB_LOG_SOURCE, "shutting down (rx=%llu tx=%llu)",
             (unsigned long long)g_buffers_received.load(),
             (unsigned long long)g_buffers_published.load());
    unified_log_shutdown();
    return 0;
}
