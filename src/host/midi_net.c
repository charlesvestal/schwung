/* midi_net.c - network MIDI lifecycle, parsing, and lock-free queues. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "midi_net_internal.h"
#include "midi_net_rtpmidi_api.h"
#include "shadow_midi_inject_writer.h"
#include "unified_log.h"

midi_net_state_t g_midi_net;

void midi_net_log(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG_INFO("midi_net", "%s", msg);
}

void midi_net_logd(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG_DEBUG("midi_net", "%s", msg);
}

uint64_t midi_net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Network input is tagged with cable 3 in the injection ring. The SPI-thread
 * drain converts it to external cable 2 before Move sees it. */
int midi_net_inject_usb_packet(uint8_t cin, uint8_t status,
                               uint8_t d1, uint8_t d2) {
    shadow_midi_inject_t **ptr = g_midi_net.inject_shm_ptr;
    shadow_midi_inject_t *shm = ptr ? *ptr : NULL;
    if (!shm) return 0;
    uint8_t pkt[4] = {
        (uint8_t)((MIDI_NET_INJECT_CABLE << 4) | (cin & 0x0f)),
        status, d1, d2
    };
    if (shadow_midi_inject_push(shm, pkt) != 0) return 0;
    return 1;
}

static int data_bytes_for_status(uint8_t status) {
    switch (status & 0xf0) {
    case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
    case 0xc0: case 0xd0: return 1;
    default:
        if (status == 0xf1 || status == 0xf3) return 1;
        if (status == 0xf2) return 2;
        if (status == 0xf6 || status >= 0xf8) return 0;
        return -1;
    }
}

int midi_net_emit_midi_message(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!(status & 0x80)) return 0;
    int need = data_bytes_for_status(status);
    if (need < 0 || (need >= 1 && (d1 & 0x80)) ||
        (need >= 2 && (d2 & 0x80))) {
        return 0;
    }

    uint8_t cin;
    switch (status & 0xf0) {
    case 0x80: cin = CIN_NOTE_OFF; break;
    case 0x90: cin = CIN_NOTE_ON; break;
    case 0xa0: cin = CIN_POLY_KEY_PRESS; break;
    case 0xb0: cin = CIN_CONTROL_CHANGE; break;
    case 0xc0: cin = CIN_PROGRAM_CHANGE; d2 = 0; break;
    case 0xd0: cin = CIN_CHANNEL_PRESS; d2 = 0; break;
    case 0xe0: cin = CIN_PITCH_BEND; break;
    default:
        if (status == 0xf1 || status == 0xf3) {
            cin = CIN_SYSTEM_COMMON_2;
            d2 = 0;
        } else if (status == 0xf2) {
            cin = CIN_SYSTEM_COMMON_3;
        } else {
            cin = CIN_SINGLE_BYTE;
            d1 = d2 = 0;
        }
        break;
    }
    return midi_net_inject_usb_packet(cin, status, d1, d2);
}

/* Reserve and publish a whole group in the existing Vyukov MPSC ring. The
 * head slot is published last, so the SPI consumer can never observe a
 * partial batch. This is used only by the non-realtime network thread. */
static int inject_usb_batch(shadow_midi_inject_t *shm,
                            const uint8_t packets[][4], uint32_t count) {
    if (!shm || !packets || count == 0 || count > SHADOW_MIDI_INJECT_SLOTS)
        return -1;

    uint32_t pos = __atomic_load_n(&shm->enqueue_pos, __ATOMIC_RELAXED);
    for (;;) {
        int all_free = 1;
        for (uint32_t i = 0; i < count; i++) {
            shadow_midi_inject_slot_t *slot =
                &shm->slots[(pos + i) & SHADOW_MIDI_INJECT_MASK];
            uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
            if ((int32_t)(seq - (pos + i)) != 0) {
                all_free = 0;
                break;
            }
        }
        if (!all_free) return -1;

        uint32_t expected = pos;
        if (__atomic_compare_exchange_n(&shm->enqueue_pos, &expected,
                                        pos + count, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) break;
        pos = expected;
    }

    for (uint32_t i = 0; i < count; i++) {
        shadow_midi_inject_slot_t *slot =
            &shm->slots[(pos + i) & SHADOW_MIDI_INJECT_MASK];
        memcpy(slot->pkt, packets[i], 4);
    }
    for (uint32_t i = count; i-- > 0;) {
        shadow_midi_inject_slot_t *slot =
            &shm->slots[(pos + i) & SHADOW_MIDI_INJECT_MASK];
        __atomic_store_n(&slot->seq, pos + i + 1, __ATOMIC_RELEASE);
    }
    return 0;
}

int midi_net_emit_sysex(const uint8_t *bytes, int len) {
    if (!bytes || len <= 0 || bytes[0] != 0xf0 || bytes[len - 1] != 0xf7)
        return 0;
    uint32_t packet_count = ((uint32_t)len + 2u) / 3u;
    if (packet_count > SHADOW_MIDI_INJECT_SLOTS) return 0;
    uint8_t packets[SHADOW_MIDI_INJECT_SLOTS][4];
    uint32_t packet_index = 0;
    for (int i = 0; i < len;) {
        int remain = len - i;
        uint8_t cin;
        uint8_t b1 = bytes[i], b2 = 0, b3 = 0;
        int take;
        if (remain > 3) {
            cin = CIN_SYSEX_START_CONT; take = 3;
        } else {
            take = remain;
            cin = (remain == 1) ? CIN_SYSEX_END_1 :
                  (remain == 2) ? CIN_SYSEX_END_2 : CIN_SYSEX_END_3;
        }
        if (take >= 2) b2 = bytes[i + 1];
        if (take >= 3) b3 = bytes[i + 2];
        packets[packet_index][0] =
            (uint8_t)((MIDI_NET_INJECT_CABLE << 4) | cin);
        packets[packet_index][1] = b1;
        packets[packet_index][2] = b2;
        packets[packet_index][3] = b3;
        packet_index++;
        i += take;
    }
    shadow_midi_inject_t **ptr = g_midi_net.inject_shm_ptr;
    shadow_midi_inject_t *shm = ptr ? *ptr : NULL;
    if (inject_usb_batch(shm, packets, packet_count) != 0) return 0;
    return (int)packet_count;
}

static void parser_begin_pending(midi_net_stream_parser_t *p, uint8_t status) {
    p->pending_status = status;
    p->pending_need = (uint8_t)data_bytes_for_status(status);
    p->pending_len = 0;
}

static int parser_emit_pending(midi_net_stream_parser_t *p) {
    uint8_t d1 = p->pending_len > 0 ? p->pending_data[0] : 0;
    uint8_t d2 = p->pending_len > 1 ? p->pending_data[1] : 0;
    int emitted = midi_net_emit_midi_message(p->pending_status, d1, d2);
    if (p->running_status) parser_begin_pending(p, p->running_status);
    else p->pending_status = p->pending_need = p->pending_len = 0;
    return emitted;
}

int midi_net_parse_raw_stream(midi_net_stream_parser_t *p,
                              const uint8_t *bytes, int len) {
    if (!p || !bytes || len <= 0) return 0;
    int emitted = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = bytes[i];

        /* Realtime may occur anywhere and never alters running status or a
         * partially collected message. */
        if (b >= 0xf8) {
            emitted += midi_net_emit_midi_message(b, 0, 0);
            continue;
        }

        if (p->in_sysex) {
            if (b == 0xf7) {
                if (!p->sysex_overflow && p->sysex_len < MIDI_NET_SYSEX_SCRATCH) {
                    p->sysex_buf[p->sysex_len++] = b;
                    emitted += midi_net_emit_sysex(p->sysex_buf, (int)p->sysex_len);
                }
                p->in_sysex = p->sysex_overflow = 0;
                p->sysex_len = 0;
                continue;
            }
            if (!(b & 0x80)) {
                if (p->sysex_len < MIDI_NET_SYSEX_SCRATCH)
                    p->sysex_buf[p->sysex_len++] = b;
                else p->sysex_overflow = 1;
                continue;
            }
            /* A non-realtime status aborts an unterminated SysEx and is then
             * processed normally. */
            p->in_sysex = p->sysex_overflow = 0;
            p->sysex_len = 0;
        }

        if (b & 0x80) {
            p->pending_status = p->pending_need = p->pending_len = 0;
            if (b == 0xf0) {
                p->running_status = 0;
                p->in_sysex = 1;
                p->sysex_len = 1;
                p->sysex_buf[0] = 0xf0;
                continue;
            }
            int need = data_bytes_for_status(b);
            if (need < 0) {
                p->running_status = 0;
                continue;
            }
            p->running_status = b < 0xf0 ? b : 0;
            if (need == 0) emitted += midi_net_emit_midi_message(b, 0, 0);
            else parser_begin_pending(p, b);
            continue;
        }

        if (!p->pending_need) {
            if (!p->running_status) continue;
            parser_begin_pending(p, p->running_status);
        }
        if (p->pending_len < sizeof(p->pending_data))
            p->pending_data[p->pending_len++] = b;
        if (p->pending_len == p->pending_need)
            emitted += parser_emit_pending(p);
    }
    return emitted;
}

/* Bounded MPSC queue: realtime producers never wait for the network consumer. */
static int outbound_push(const uint8_t pkt[4], uint32_t generation) {
    uint32_t pos = __atomic_load_n(&g_midi_net.outbound_enqueue_pos,
                                   __ATOMIC_RELAXED);
    midi_net_outbound_slot_t *slot;
    for (;;) {
        slot = &g_midi_net.outbound[pos & MIDI_NET_OUTBOUND_MASK];
        uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        int32_t diff = (int32_t)(seq - pos);
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&g_midi_net.outbound_enqueue_pos,
                                            &pos, pos + 1, 1,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) break;
        } else if (diff < 0) {
            return -1;
        } else {
            pos = __atomic_load_n(&g_midi_net.outbound_enqueue_pos,
                                  __ATOMIC_RELAXED);
        }
    }
    memcpy(slot->pkt, pkt, 4);
    slot->generation = generation;
    __atomic_store_n(&slot->seq, pos + 1, __ATOMIC_RELEASE);
    return 0;
}

static int outbound_pop(uint8_t pkt[4], uint32_t *generation) {
    uint32_t pos = g_midi_net.outbound_read_pos;
    midi_net_outbound_slot_t *slot =
        &g_midi_net.outbound[pos & MIDI_NET_OUTBOUND_MASK];
    uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if ((int32_t)(seq - (pos + 1)) != 0) return 0;
    memcpy(pkt, slot->pkt, 4);
    if (generation) *generation = slot->generation;
    __atomic_store_n(&slot->seq, pos + MIDI_NET_OUTBOUND_SLOTS,
                     __ATOMIC_RELEASE);
    g_midi_net.outbound_read_pos = pos + 1;
    return 1;
}

static void outbound_reset(void) {
    g_midi_net.outbound_enqueue_pos = 0;
    g_midi_net.outbound_read_pos = 0;
    for (uint32_t i = 0; i < MIDI_NET_OUTBOUND_SLOTS; i++)
        __atomic_store_n(&g_midi_net.outbound[i].seq, i, __ATOMIC_RELAXED);
}

void midi_net_publish(const uint8_t pkt4[4]) {
    if (!pkt4) return;
    /* Load the generation before the running gate. stop() publishes false
     * before advancing the generation, so a producer either rejects the
     * packet or tags it with the old generation. It can never leak a late
     * packet into the next service instance. */
    uint32_t generation = __atomic_load_n(&g_midi_net.service_generation,
                                           __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE)) return;
    (void)outbound_push(pkt4, generation);
}

static int outbound_pop_current(uint8_t pkt[4]) {
    uint32_t packet_generation;
    while (outbound_pop(pkt, &packet_generation)) {
        uint32_t current = __atomic_load_n(&g_midi_net.service_generation,
                                           __ATOMIC_ACQUIRE);
        if (packet_generation == current) return 1;
    }
    return 0;
}

static void configure_network_thread(void) {
    struct sched_param sp = { .sched_priority = 0 };
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef __linux__
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask); CPU_SET(1, &mask); CPU_SET(2, &mask);
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
#endif
}

static int adapter_is_running(void *unused) {
    (void)unused;
    return __atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE);
}

static void adapter_handle_ipmidi(void *unused, int fd) {
    (void)unused;
    midi_net_ipmidi_handle_rx(fd);
}

static int adapter_pop_outbound(void *unused, uint8_t packet[4]) {
    (void)unused;
    return outbound_pop_current(packet);
}

static void adapter_handle_inbound(void *unused, const uint8_t *bytes,
                                   size_t len) {
    (void)unused;
    midi_net_stream_parser_t parser;
    memset(&parser, 0, sizeof(parser));
    midi_net_parse_raw_stream(&parser, bytes, (int)len);
}

static void adapter_log(void *unused, int is_error, const char *message) {
    (void)unused;
    if (is_error) LOG_ERROR("midi_net", "%s", message ? message : "rtpmidi error");
    else LOG_INFO("midi_net", "%s", message ? message : "rtpmidi");
}

static void fallback_poll_loop(void) {
    while (__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE)) {
        struct pollfd pfds[2];
        int nfds = 0, idx_stop = -1, idx_ip = -1;
#define ADD_FD(fd_, idx_) do { if ((fd_) >= 0) { \
            (idx_) = nfds; pfds[nfds].fd = (fd_); \
            pfds[nfds].events = POLLIN; pfds[nfds].revents = 0; nfds++; \
        } } while (0)
        ADD_FD(g_midi_net.self_pipe[0], idx_stop);
        ADD_FD(g_midi_net.ipmidi_sock, idx_ip);
#undef ADD_FD
        int rc = poll(pfds, (nfds_t)nfds, 100);
        if (rc < 0 && errno != EINTR) {
            midi_net_log("fallback poll failed: %s", strerror(errno));
            break;
        }
        if (idx_stop >= 0 && (pfds[idx_stop].revents & POLLIN)) {
            char buf[16];
            while (read(g_midi_net.self_pipe[0], buf, sizeof(buf)) > 0) { }
        }
        if (idx_ip >= 0 && (pfds[idx_ip].revents & POLLIN))
            midi_net_ipmidi_handle_rx(g_midi_net.ipmidi_sock);
    }
}

static void *network_main(void *unused) {
    (void)unused;
    configure_network_thread();
    midi_net_log("network MIDI thread starting");

    midi_net_ipmidi_open();

    if (g_midi_net.rtpmidi_run) {
        schwung_rtpmidi_config_t config = {
            .abi_version = SCHWUNG_RTPMIDI_ADAPTER_ABI,
            .session_name = MIDI_NET_SESSION_NAME,
            .control_port = 5004,
            .stop_fd = g_midi_net.self_pipe[0],
            .ipmidi_fd = g_midi_net.ipmidi_sock,
            .userdata = NULL,
            .is_running = adapter_is_running,
            .handle_ipmidi = adapter_handle_ipmidi,
            .pop_outbound = adapter_pop_outbound,
            .handle_inbound = adapter_handle_inbound,
            .log = adapter_log,
        };
        if (g_midi_net.rtpmidi_run(&config) != 0) {
            midi_net_log("RTP-MIDI adapter stopped with an error; keeping ipMIDI active");
            fallback_poll_loop();
        }
    } else {
        fallback_poll_loop();
    }

    midi_net_ipmidi_close();
    midi_net_log("network MIDI thread stopped");
    return NULL;
}

void midi_net_init(shadow_midi_inject_t **inject_shm_ptr) {
    memset(&g_midi_net, 0, sizeof(g_midi_net));
    g_midi_net.inject_shm_ptr = inject_shm_ptr;
    g_midi_net.self_pipe[0] = g_midi_net.self_pipe[1] = -1;
    g_midi_net.ipmidi_sock = -1;
    g_midi_net.service_generation = 1;
    outbound_reset();
}

static void load_rtpmidi_adapter(void) {
#ifndef MIDI_NET_TESTING
    if (g_midi_net.rtpmidi_dso) return;
    g_midi_net.rtpmidi_dso = dlopen("libschwung-rtpmidi.so",
                                    RTLD_NOW | RTLD_LOCAL);
    if (!g_midi_net.rtpmidi_dso) {
        midi_net_log("RTP-MIDI adapter unavailable: %s", dlerror());
        return;
    }
    dlerror();
    *(void **)(&g_midi_net.rtpmidi_run) =
        dlsym(g_midi_net.rtpmidi_dso, "schwung_rtpmidi_run");
    const char *error = dlerror();
    if (error || !g_midi_net.rtpmidi_run) {
        midi_net_log("RTP-MIDI adapter has no compatible entry point: %s",
                     error ? error : "missing symbol");
        dlclose(g_midi_net.rtpmidi_dso);
        g_midi_net.rtpmidi_dso = NULL;
        g_midi_net.rtpmidi_run = NULL;
    }
#endif
}

static int midi_net_start(void) {
    if (__atomic_load_n(&g_midi_net.thread_started, __ATOMIC_ACQUIRE)) return 0;
    load_rtpmidi_adapter();
    if (pipe(g_midi_net.self_pipe) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(g_midi_net.self_pipe[i], F_GETFL, 0);
        if (flags >= 0) fcntl(g_midi_net.self_pipe[i], F_SETFL, flags | O_NONBLOCK);
    }
    __atomic_store_n(&g_midi_net.running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_midi_net.thread, NULL, network_main, NULL) != 0) {
        __atomic_store_n(&g_midi_net.running, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_midi_net.service_generation, 1,
                           __ATOMIC_ACQ_REL);
        close(g_midi_net.self_pipe[0]); close(g_midi_net.self_pipe[1]);
        g_midi_net.self_pipe[0] = g_midi_net.self_pipe[1] = -1;
        return -1;
    }
    __atomic_store_n(&g_midi_net.thread_started, 1, __ATOMIC_RELEASE);
    return 0;
}

static void midi_net_stop(void) {
    if (!__atomic_load_n(&g_midi_net.thread_started, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_midi_net.running, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_midi_net.service_generation, 1,
                       __ATOMIC_ACQ_REL);
    if (g_midi_net.self_pipe[1] >= 0) {
        uint8_t byte = 1;
        (void)write(g_midi_net.self_pipe[1], &byte, 1);
    }
    pthread_join(g_midi_net.thread, NULL);
    close(g_midi_net.self_pipe[0]); close(g_midi_net.self_pipe[1]);
    g_midi_net.self_pipe[0] = g_midi_net.self_pipe[1] = -1;
    __atomic_store_n(&g_midi_net.thread_started, 0, __ATOMIC_RELEASE);
}

static int midi_net_is_running(void) {
    return __atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE);
}

void midi_net_reconcile(int enabled) {
    if (enabled) {
        if (!midi_net_is_running() && midi_net_start() != 0)
            midi_net_log("failed to start network MIDI service");
    } else if (midi_net_is_running()) {
        midi_net_stop();
    }
}

#ifdef MIDI_NET_TESTING
int midi_net_test_outbound_pop(uint8_t pkt4[4]) {
    return outbound_pop(pkt4, NULL);
}
int midi_net_test_outbound_pop_current(uint8_t pkt4[4]) {
    return outbound_pop_current(pkt4);
}
#endif
