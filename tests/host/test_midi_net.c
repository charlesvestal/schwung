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
static int fake_adapter_runs;
static int fake_adapter_outbound;
static int fake_adapter_config_ok;

static int fake_rtpmidi_run(const schwung_rtpmidi_config_t *config) {
    if (!config || config->abi_version != SCHWUNG_RTPMIDI_ADAPTER_ABI ||
        config->control_port != 5004 || config->stop_fd < 0 ||
        !config->is_running || !config->handle_ipmidi ||
        !config->pop_outbound || !config->handle_inbound)
        return -1;
    __atomic_store_n(&fake_adapter_config_ok, 1, __ATOMIC_RELEASE);

    __atomic_fetch_add(&fake_adapter_runs, 1, __ATOMIC_ACQ_REL);
    const uint8_t inbound[] = { 0x90, 64, 100 };
    config->handle_inbound(config->userdata, inbound, sizeof(inbound));

    while (config->is_running(config->userdata)) {
        uint8_t packet[4];
        if (config->pop_outbound(config->userdata, packet)) {
            if ((packet[0] & 0x0F) == CIN_NOTE_ON && packet[1] == 0x90 &&
                packet[2] == 65 && packet[3] == 110)
                __atomic_store_n(&fake_adapter_outbound, 1,
                                 __ATOMIC_RELEASE);
        } else {
            sched_yield();
        }
    }
    return 0;
}

static int wait_for_value(const int *value, int expected) {
    for (int i = 0; i < 100000; i++) {
        if (__atomic_load_n(value, __ATOMIC_ACQUIRE) >= expected) return 1;
        sched_yield();
    }
    return 0;
}

static int pop_injected(uint8_t out[4]) {
    if (!shadow_midi_inject_peek(&inject_ring, out)) return 0;
    shadow_midi_inject_pop(&inject_ring);
    return 1;
}

static void reset_service(void) {
    shadow_midi_inject_init(&inject_ring);
    midi_net_init(&inject_ptr);
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
    fake_adapter_runs = 0;
    fake_adapter_outbound = 0;
    fake_adapter_config_ok = 0;
    g_midi_net.rtpmidi_run = fake_rtpmidi_run;
    midi_net_reconcile(1);
    CHECK(__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE),
          "network service starts");
    CHECK(wait_for_value(&fake_adapter_runs, 1),
          "fake adapter starts on the network thread");
    CHECK(__atomic_load_n(&fake_adapter_config_ok, __ATOMIC_ACQUIRE),
          "fake adapter receives the expected C ABI configuration");

    const uint8_t outbound[] = { 0x29, 0x90, 65, 110 };
    midi_net_publish(outbound);
    CHECK(wait_for_value(&fake_adapter_outbound, 1),
          "fake adapter drains realtime outbound publication");
    uint8_t packet[4];
    CHECK(pop_injected(packet) && packet[1] == 0x90 && packet[2] == 64 &&
          packet[3] == 100,
          "fake adapter inbound callback uses the injection conversion");

    midi_net_reconcile(0);
    CHECK(!__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE),
          "network service stops and joins");
    midi_net_reconcile(1);
    CHECK(__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE),
          "network service can restart cleanly");
    CHECK(wait_for_value(&fake_adapter_runs, 2),
          "fake adapter is reconstructed after restart");
    midi_net_reconcile(0);
    CHECK(!__atomic_load_n(&g_midi_net.running, __ATOMIC_ACQUIRE),
          "restarted network service stops cleanly");
}

int main(void) {
    test_raw_stream_state();
    test_sysex_across_datagrams();
    test_sysex_batch_is_atomic();
    test_sysex_batch_concurrent_producer();
    test_outbound_queue();
    test_service_lifecycle();
    if (failures) {
        fprintf(stderr, "%d network MIDI test(s) failed\n", failures);
        return 1;
    }
    puts("network MIDI tests passed");
    return 0;
}
