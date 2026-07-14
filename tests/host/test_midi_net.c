#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "midi_net_internal.h"
#include "shadow_midi_inject_writer.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static shadow_midi_inject_t inject_ring;
static shadow_midi_inject_t *inject_ptr = &inject_ring;

static int pop_injected(uint8_t out[4]) {
    if (!shadow_midi_inject_peek(&inject_ring, out)) return 0;
    shadow_midi_inject_pop(&inject_ring);
    return 1;
}

static void reset_service(void) {
    shadow_midi_inject_init(&inject_ring);
    midi_net_init(&inject_ptr, NULL);
}

static void test_raw_stream_state(void) {
    reset_service();
    midi_net_stream_parser_t parser;
    memset(&parser, 0, sizeof(parser));
    const uint8_t a[] = { 0x90, 60 };
    const uint8_t b[] = { 100, 61, 0xF8, 110 };
    CHECK(midi_net_parse_raw_stream(&parser, a, sizeof(a)) == 0,
          "partial channel message is retained");
    CHECK(midi_net_parse_raw_stream(&parser, b, sizeof(b)) == 3,
          "running status and interleaved realtime emit three messages");

    uint8_t pkt[4];
    CHECK(pop_injected(pkt) && pkt[0] == 0x39 && pkt[1] == 0x90 &&
          pkt[2] == 60 && pkt[3] == 100, "first note packet");
    CHECK(pop_injected(pkt) && pkt[0] == 0x3F && pkt[1] == 0xF8,
          "realtime packet preserves running status");
    CHECK(pop_injected(pkt) && pkt[1] == 0x90 && pkt[2] == 61 &&
          pkt[3] == 110, "running-status note packet");
    CHECK(!pop_injected(pkt), "no extra raw-stream packets");
}

static void test_sysex_across_datagrams(void) {
    reset_service();
    midi_net_stream_parser_t parser;
    memset(&parser, 0, sizeof(parser));
    const uint8_t a[] = { 0xF0, 1, 2 };
    const uint8_t b[] = { 3, 0xF7 };
    CHECK(midi_net_parse_raw_stream(&parser, a, sizeof(a)) == 0,
          "partial SysEx is retained");
    CHECK(midi_net_parse_raw_stream(&parser, b, sizeof(b)) == 2,
          "completed SysEx emits two USB-MIDI fragments");
    uint8_t pkt[4];
    CHECK(pop_injected(pkt) && pkt[0] == 0x34 && pkt[1] == 0xF0 &&
          pkt[2] == 1 && pkt[3] == 2, "SysEx start fragment");
    CHECK(pop_injected(pkt) && pkt[0] == 0x36 && pkt[1] == 3 &&
          pkt[2] == 0xF7, "SysEx end fragment");
}

static void test_sysex_batch_is_atomic(void) {
    reset_service();
    for (uint32_t i = 0; i < SHADOW_MIDI_INJECT_SLOTS - 1; i++)
        CHECK(midi_net_inject_usb_packet(CIN_NOTE_ON, 0x90,
                                        (uint8_t)i, 100),
              "pre-fill injection ring");
    const uint8_t sysex[] = { 0xF0, 1, 2, 3, 0xF7 };
    CHECK(midi_net_emit_sysex(sysex, sizeof(sysex)) == 0,
          "SysEx is rejected when its whole batch cannot fit");

    uint8_t pkt[4];
    uint32_t count = 0;
    while (pop_injected(pkt)) count++;
    CHECK(count == SHADOW_MIDI_INJECT_SLOTS - 1,
          "failed SysEx leaves no partial packets in the ring");
}

static void *single_packet_producer(void *unused) {
    (void)unused;
    for (int i = 0; i < 4000; i++) {
        while (!midi_net_inject_usb_packet(CIN_NOTE_ON, 0x90,
                                           (uint8_t)(i & 0x7F), 100))
            sched_yield();
    }
    return NULL;
}

static void *sysex_batch_producer(void *unused) {
    (void)unused;
    const uint8_t sysex[] = { 0xF0, 1, 2, 3, 0xF7 };
    for (int i = 0; i < 1000; i++) {
        while (midi_net_emit_sysex(sysex, sizeof(sysex)) != 2)
            sched_yield();
    }
    return NULL;
}

static void test_sysex_batch_concurrent_producer(void) {
    reset_service();
    pthread_t single_thread, sysex_thread;
    int single_rc = pthread_create(&single_thread, NULL,
                                   single_packet_producer, NULL);
    int sysex_rc = pthread_create(&sysex_thread, NULL,
                                  sysex_batch_producer, NULL);
    CHECK(single_rc == 0,
          "start single-packet producer");
    CHECK(sysex_rc == 0,
          "start SysEx-batch producer");
    if (single_rc != 0 || sysex_rc != 0) {
        if (single_rc == 0) pthread_join(single_thread, NULL);
        if (sysex_rc == 0) pthread_join(sysex_thread, NULL);
        return;
    }

    int singles = 0, sysexes = 0;
    uint8_t pkt[4];
    while (singles < 4000 || sysexes < 1000) {
        if (!pop_injected(pkt)) {
            sched_yield();
            continue;
        }
        if ((pkt[0] & 0x0F) == CIN_NOTE_ON) {
            singles++;
            continue;
        }
        CHECK((pkt[0] & 0x0F) == CIN_SYSEX_START_CONT &&
              pkt[1] == 0xF0 && pkt[2] == 1 && pkt[3] == 2,
              "concurrent SysEx batch starts intact");
        while (!pop_injected(pkt)) sched_yield();
        CHECK((pkt[0] & 0x0F) == CIN_SYSEX_END_2 &&
              pkt[1] == 3 && pkt[2] == 0xF7,
              "concurrent SysEx batch remains contiguous");
        sysexes++;
    }
    pthread_join(single_thread, NULL);
    pthread_join(sysex_thread, NULL);
    CHECK(!pop_injected(pkt), "concurrent producer test drains the ring");
}

static void test_rtp_midi_parser(void) {
    reset_service();
    midi_net_peer_t peer;
    memset(&peer, 0, sizeof(peer));
    const uint8_t packet[] = {
        0x80, 0x61, 0, 1, 0, 0, 0, 1, 0x11, 0x22, 0x33, 0x44,
        6, 0x90, 60, 100, 0, 61, 110
    };
    midi_net_test_parse_rtp(&peer, packet, sizeof(packet));
    uint8_t pkt[4];
    CHECK(pop_injected(pkt) && pkt[1] == 0x90 && pkt[2] == 60 &&
          pkt[3] == 100, "RTP first command");
    CHECK(pop_injected(pkt) && pkt[1] == 0x90 && pkt[2] == 61 &&
          pkt[3] == 110, "RTP delta + running-status command");
    CHECK(!pop_injected(pkt), "no extra RTP packets");

    const uint8_t truncated[] = {
        0x80, 0x61, 0, 2, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44,
        15, 0x90
    };
    midi_net_test_parse_rtp(&peer, truncated, sizeof(truncated));
    CHECK(!pop_injected(pkt), "truncated RTP command section is rejected");

    const uint8_t p_running[] = {
        0x80, 0x61, 0, 3, 0, 0, 0, 3, 0x11, 0x22, 0x33, 0x44,
        0x12, 62, 120
    };
    midi_net_test_parse_rtp(&peer, p_running, sizeof(p_running));
    CHECK(pop_injected(pkt) && pkt[1] == 0x90 && pkt[2] == 62 &&
          pkt[3] == 120, "RTP P flag continues prior-packet running status");

    const uint8_t missing_p[] = {
        0x80, 0x61, 0, 4, 0, 0, 0, 4, 0x11, 0x22, 0x33, 0x44,
        2, 63, 120
    };
    midi_net_test_parse_rtp(&peer, missing_p, sizeof(missing_p));
    CHECK(!pop_injected(pkt),
          "RTP data without P flag cannot reuse prior-packet running status");
}

static void test_rtp_sysex_segments(void) {
    reset_service();
    midi_net_peer_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.peer_ssrc = 0x11223344;
    const uint8_t first[] = {
        0x80, 0x61, 0, 1, 0, 0, 0, 1, 0x11, 0x22, 0x33, 0x44,
        4, 0xF0, 1, 2, 0xF0
    };
    const uint8_t last[] = {
        0x80, 0x61, 0, 2, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44,
        3, 0xF7, 3, 0xF7
    };
    midi_net_test_parse_rtp(&peer, first, sizeof(first));
    uint8_t pkt[4];
    CHECK(!pop_injected(pkt), "RTP SysEx first segment is retained");
    midi_net_test_parse_rtp(&peer, last, sizeof(last));
    CHECK(pop_injected(pkt) && pkt[0] == 0x34 && pkt[1] == 0xF0 &&
          pkt[2] == 1 && pkt[3] == 2, "RTP segmented SysEx start");
    CHECK(pop_injected(pkt) && pkt[0] == 0x36 && pkt[1] == 3 &&
          pkt[2] == 0xF7, "RTP segmented SysEx end");
    CHECK(!pop_injected(pkt), "no extra RTP SysEx packets");
}

static void test_dns_compression_loop(void) {
    const uint8_t loop[] = { 0xC0, 0x00 };
    char out[32];
    CHECK(midi_net_test_decode_dns_name(loop, sizeof(loop), 0,
                                        out, sizeof(out)) < 0,
          "mDNS compression pointer loop is rejected");
}

static void test_outbound_queue(void) {
    reset_service();
    __atomic_store_n(&g_midi_net.running, 1, __ATOMIC_RELEASE);
    uint8_t pkt[4];
    for (uint32_t i = 0; i < MIDI_NET_OUTBOUND_SLOTS; i++) {
        const uint8_t in[4] = { 0x29, 0x90, (uint8_t)(i & 0x7F), 100 };
        midi_net_publish(in);
    }
    const uint8_t overflow[4] = { 0x29, 0x90, 127, 127 };
    midi_net_publish(overflow);
    CHECK(g_midi_net.stats.drops_outbound_full == 1,
          "outbound queue drops newest packet when full");
    for (uint32_t i = 0; i < MIDI_NET_OUTBOUND_SLOTS; i++) {
        CHECK(midi_net_test_outbound_pop(pkt), "outbound packet available");
        CHECK(pkt[2] == (uint8_t)(i & 0x7F), "outbound FIFO ordering");
    }
    CHECK(!midi_net_test_outbound_pop(pkt), "outbound queue drains empty");

    const uint8_t stale[4] = { 0x29, 0x90, 12, 34 };
    midi_net_publish(stale);
    __atomic_store_n(&g_midi_net.running, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_midi_net.service_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_midi_net.running, 1, __ATOMIC_RELEASE);
    CHECK(!midi_net_test_outbound_pop_current(pkt),
          "service restart discards old-generation outbound MIDI");
    __atomic_store_n(&g_midi_net.running, 0, __ATOMIC_RELEASE);
}

static void test_service_lifecycle(void) {
    reset_service();
    midi_net_set_backends(0);
    CHECK(midi_net_start() == 0 && midi_net_is_running(),
          "network service starts without backends");
    midi_net_stop();
    CHECK(!midi_net_is_running(), "network service stops and joins");
    CHECK(midi_net_start() == 0 && midi_net_is_running(),
          "network service can restart cleanly");
    midi_net_stop();
    CHECK(!midi_net_is_running(), "restarted network service stops cleanly");
}

int main(void) {
    test_raw_stream_state();
    test_sysex_across_datagrams();
    test_sysex_batch_is_atomic();
    test_sysex_batch_concurrent_producer();
    test_rtp_midi_parser();
    test_rtp_sysex_segments();
    test_dns_compression_loop();
    test_outbound_queue();
    test_service_lifecycle();
    if (failures) {
        fprintf(stderr, "%d network MIDI test(s) failed\n", failures);
        return 1;
    }
    puts("network MIDI tests passed");
    return 0;
}
