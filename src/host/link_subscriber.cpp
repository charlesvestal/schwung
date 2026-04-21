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
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
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
    std::ifstream f("/data/UserData/schwung/config/features.json");
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

/* Create (or open if already existing) the Move->shim audio SHM segment.
 * Written by the link-subscriber source callback (future Phase 2.x task),
 * read by the shim to mix Move audio into shadow output. */
static link_audio_in_shm_t *open_or_create_in_shm()
{
    int fd = shm_open(SHM_LINK_AUDIO_IN, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, sizeof(link_audio_in_shm_t)) < 0) { close(fd); return nullptr; }
    auto *shm = (link_audio_in_shm_t *)mmap(nullptr, sizeof(link_audio_in_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) return nullptr;
    /* Re-init on EITHER a missing magic (fresh segment) OR a stale version
     * (previous sidecar left a different layout). Without the version
     * check, a shim upgrade that added fields would leave the old segment
     * intact and the new shim would refuse to attach. */
    if (shm->magic != LINK_AUDIO_IN_SHM_MAGIC ||
        shm->version != LINK_AUDIO_IN_SHM_VERSION) {
        memset(shm, 0, sizeof(*shm));
        shm->magic = LINK_AUDIO_IN_SHM_MAGIC;
        shm->version = LINK_AUDIO_IN_SHM_VERSION;
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

    /* Read last-known session tempo. Written by our own setTempoCallback so
     * each subsequent launch reuses the tempo that was last agreed upon,
     * avoiding a default-120 proposal that would hijack the session if we
     * happen to win the Link session-merge. */
    const char *LAST_TEMPO_PATH = "/data/UserData/schwung/last-tempo";
    double initial_tempo = 120.0;
    {
        FILE *fp = fopen(LAST_TEMPO_PATH, "r");
        if (fp) {
            double t = 0.0;
            if (fscanf(fp, "%lf", &t) == 1 && t >= 20.0 && t <= 999.0) {
                initial_tempo = t;
                printf("link-subscriber: using last-known tempo %.2f BPM\n", t);
            }
            fclose(fp);
        }
        if (initial_tempo == 120.0) {
            printf("link-subscriber: no last-known tempo, defaulting to 120.0 BPM\n");
        }
    }

    /* Join Link session and enable audio */
    ableton::LinkAudio link(initial_tempo, "Schwung");
    link.setTempoCallback([LAST_TEMPO_PATH](double bpm) {
        /* Persist each session-tempo change so the next subscriber launch
         * inherits it. Link fires this on our own thread (non-RT).
         * Write-then-rename so a crash mid-write can't leave a
         * half-written number that fails the fscanf validation on the
         * next launch. */
        std::string tmp = std::string(LAST_TEMPO_PATH) + ".tmp";
        FILE *fp = fopen(tmp.c_str(), "w");
        if (fp) {
            fprintf(fp, "%.4f\n", bpm);
            fclose(fp);
            (void)rename(tmp.c_str(), LAST_TEMPO_PATH);
        }
    });
    link.enable(true);
    link.enableLinkAudio(true);

    printf("link-subscriber: Link session joined\n");

    /* Create Move->shim audio SHM segment. No consumers yet — Phase 2.x will
     * teach the source callback to write samples into this ring. */
    link_audio_in_shm_t *in_shm = open_or_create_in_shm();
    if (in_shm) {
        LOG_INFO(LINK_SUB_LOG_SOURCE, "in shm opened/created");
    } else {
        LOG_ERROR(LINK_SUB_LOG_SOURCE, "failed to open/create in shm: errno=%d", errno);
    }
    (void)in_shm; /* Phase 2.x will consume this */

    /* Create a dummy sink so that our PeerAnnouncements include at least one
     * channel.  Move's Sink handler looks up ChannelRequest.peerId in
     * mPeerSendHandlers, which is only populated when a PeerAnnouncement
     * with channels is received.  Without this, forPeer() returns nullopt
     * and audio is silently never sent. */
    ableton::LinkAudioSink dummySink(link, "Schwung-Ack", 256);
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

    /* Tempo override protocol: shim/UI writes desired BPM to this file on set
     * change; we pick it up here and propose it to the session — but only when
     * numPeers() == 1 (Move alone), so we never clobber collaboration. */
    const char *DESIRED_TEMPO_PATH = "/data/UserData/schwung/desired-tempo";
    /* Seed with the file's current mtime so a leftover from a previous session
     * (only rewritten on set-switch, not on in-set tempo edits) doesn't replay
     * and clobber the freshly-loaded last-tempo on boot. */
    time_t last_desired_mtime = 0;
    {
        struct stat st;
        if (stat(DESIRED_TEMPO_PATH, &st) == 0) last_desired_mtime = st.st_mtime;
    }

    while (g_running) {
        /* Use a shorter sleep to poll the publisher shm more frequently.
         * The shim writes at ~344 Hz (every ~2.9ms). We poll at ~100 Hz
         * which means ~3 render blocks accumulate between polls.
         * The SDK handles the 128→125 repacketing internally. */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        tick++;

        /* Poll for tempo-override requests (set-change on device). */
        {
            struct stat st;
            if (stat(DESIRED_TEMPO_PATH, &st) == 0 && st.st_mtime != last_desired_mtime) {
                last_desired_mtime = st.st_mtime;
                FILE *fp = fopen(DESIRED_TEMPO_PATH, "r");
                if (fp) {
                    double bpm = 0.0;
                    if (fscanf(fp, "%lf", &bpm) == 1 && bpm >= 20.0 && bpm <= 999.0) {
                        size_t peers = link.numPeers();
                        if (peers <= 1) {
                            /* peers==0: alone, or Move hasn't joined yet —
                             * our proposal will be the session tempo.
                             * peers==1: just Move — force the set's tempo. */
                            auto state = link.captureAppSessionState();
                            state.setTempo(bpm, link.clock().micros());
                            link.commitAppSessionState(state);
                            LOG_INFO(LINK_SUB_LOG_SOURCE,
                                     "tempo override applied: %.2f BPM (peers=%zu)",
                                     bpm, peers);
                        } else {
                            LOG_INFO(LINK_SUB_LOG_SOURCE,
                                     "tempo override skipped: %.2f BPM requested, peers=%zu",
                                     bpm, peers);
                        }
                    }
                    fclose(fp);
                }
            }
        }

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

            /* Clear slot active flags before tearing down sources. Tracks
             * that disappeared this cycle won't be re-subscribed → stay
             * active=0 → shim falls through to native passthrough for that
             * slot instead of reading a stale silent ring forever. Slots
             * that ARE re-subscribed have active re-set by the source
             * callback on its first write. Relaxed atomics are fine — the
             * shim reads active with __sync_synchronize() anyway. */
            if (in_shm) {
                for (int i = 0; i < LINK_AUDIO_IN_SLOT_COUNT; i++) {
                    __atomic_store_n(&in_shm->slots[i].active, 0,
                                     __ATOMIC_RELAXED);
                }
            }

            sources.clear();
            LOG_INFO(LINK_SUB_LOG_SOURCE, "cleared old sources");

            /* Small delay to let SDK process the unsubscriptions */
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            sources.reserve(pending.size());

            for (const auto& pc : pending) {
                LOG_INFO(LINK_SUB_LOG_SOURCE, "subscribing to %s/%s...",
                         pc.peerName.c_str(), pc.name.c_str());

                /* Resolve Move channels to fixed slot indices. Non-Move peers
                 * get no slot (they're still subscribed for other reasons, but
                 * don't feed /schwung-link-in). */
                /* Move publishes per-track audio with track-type suffixes:
                 * "1-MIDI" / "2-MIDI" / ... for MIDI tracks, "1-Audio" / ...
                 * for audio tracks (which Move 2.0 sets can have a mix of).
                 * The leading digit identifies the track and is enough to
                 * pick the slot regardless of the suffix. */
                int slot_idx = -1;
                if (pc.peerName == "Move" && pc.name.size() >= 2) {
                    char d = pc.name[0];
                    if (d >= '1' && d <= '4' && pc.name[1] == '-') {
                        slot_idx = d - '1';
                    } else if (pc.name == "Main") {
                        slot_idx = LINK_AUDIO_IN_MAIN_IDX;
                    }
                }

                if (slot_idx >= 0 && slot_idx < LINK_AUDIO_IN_SLOT_COUNT && in_shm) {
                    link_audio_in_slot_t *slot = &in_shm->slots[slot_idx];
                    /* First time we see this slot, stamp the name. */
                    if (slot->name[0] == '\0') {
                        strncpy(slot->name, pc.name.c_str(), sizeof(slot->name) - 1);
                        slot->name[sizeof(slot->name) - 1] = '\0';
                        LOG_INFO(LINK_SUB_LOG_SOURCE, "slot %d \xe2\x86\x90 Move|%s",
                                 slot_idx, pc.name.c_str());
                    }
                }

                try {
                    if (slot_idx >= 0 && slot_idx < LINK_AUDIO_IN_SLOT_COUNT && in_shm) {
                        /* Move channel: write received audio into the per-slot
                         * SPSC ring in /schwung-link-in so the shim (or future
                         * consumer) can mix it with shadow output.
                         *
                         * Callback runs on a Link-managed audio thread.
                         * MUST be realtime-safe: no logging, no allocation,
                         * no locks. Only lock-free ring writes + atomics. */
                        int slot_idx_cap = slot_idx;
                        link_audio_in_shm_t *in_shm_cap = in_shm;
                        sources.emplace_back(link, pc.id,
                            [slot_idx_cap, in_shm_cap](ableton::LinkAudioSource::BufferHandle h) {
                                g_buffers_received.fetch_add(1, std::memory_order_relaxed);

                                const size_t num_frames   = h.info.numFrames;
                                const size_t num_channels = h.info.numChannels;
                                const int16_t *samples    = h.samples;

                                /* Drop non-stereo / empty / null buffers. */
                                if (num_channels != 2) return;
                                if (num_frames == 0) return;
                                if (!samples) return;

                                link_audio_in_slot_t *slot =
                                    &in_shm_cap->slots[slot_idx_cap];

                                const uint32_t to_copy =
                                    (uint32_t)(num_frames * num_channels); /* samples */
                                uint32_t wp = slot->write_pos;
                                uint32_t rp = __atomic_load_n(&slot->read_pos,
                                                              __ATOMIC_ACQUIRE);
                                /* Producer telemetry: count overwrites of
                                 * un-read data. Diagnostic only; we still
                                 * write (matches pre-v2 behavior). */
                                uint32_t pending = wp - rp;
                                if (pending + to_copy > LINK_AUDIO_IN_RING_SAMPLES) {
                                    __atomic_fetch_add(&slot->would_overrun_count,
                                                       1, __ATOMIC_RELAXED);
                                }
                                /* Use a volatile pointer + explicit memory fence.
                                 * The non-volatile ring[] array is otherwise
                                 * "unobservable" in this TU, so the compiler can
                                 * (and does, with -O3) elide the stores as dead. */
                                volatile int16_t *ring = slot->ring;
                                for (uint32_t i = 0; i < to_copy; ++i) {
                                    ring[(wp + i) & LINK_AUDIO_IN_RING_MASK] =
                                        samples[i];
                                }
                                __sync_synchronize();
                                __atomic_store_n(&slot->write_pos, wp + to_copy,
                                                 __ATOMIC_RELEASE);
                                __atomic_store_n(&slot->active, 1,
                                                 __ATOMIC_RELAXED);
                                __atomic_fetch_add(&slot->produced_count, 1,
                                                   __ATOMIC_RELAXED);
                                uint32_t nframes_u32 = (uint32_t)num_frames;
                                uint32_t prev_max = __atomic_load_n(
                                    &slot->max_frames_seen, __ATOMIC_RELAXED);
                                if (nframes_u32 > prev_max) {
                                    __atomic_store_n(&slot->max_frames_seen,
                                                     nframes_u32,
                                                     __ATOMIC_RELAXED);
                                }
                            });
                    } else {
                        /* Non-Move channel (e.g. ME-Ack loopback) — keep the
                         * original no-op so we don't regress the session /
                         * peer-announcement keepalive path. */
                        sources.emplace_back(link, pc.id,
                            [](ableton::LinkAudioSource::BufferHandle) {
                                g_buffers_received.fetch_add(1, std::memory_order_relaxed);
                            });
                    }
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
                        snprintf(name, sizeof(name), "Schwung-Master");
                    else
                        snprintf(name, sizeof(name), "Schwung-%d", i + 1);
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
                            snprintf(name, sizeof(name), "Schwung-Master");
                        else
                            snprintf(name, sizeof(name), "Schwung-%d", i + 1);
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
