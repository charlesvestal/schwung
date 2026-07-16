/* midi_net_internal.h - internal state shared by network MIDI backends. */

#ifndef MIDI_NET_INTERNAL_H
#define MIDI_NET_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>

#include "midi_net.h"
#include "midi_net_rtpmidi_api.h"

#define CIN_SYSTEM_COMMON_2  0x02
#define CIN_SYSTEM_COMMON_3  0x03
#define CIN_SYSEX_START_CONT 0x04
#define CIN_SYSEX_END_1      0x05
#define CIN_SYSEX_END_2      0x06
#define CIN_SYSEX_END_3      0x07
#define CIN_NOTE_OFF         0x08
#define CIN_NOTE_ON          0x09
#define CIN_POLY_KEY_PRESS   0x0A
#define CIN_CONTROL_CHANGE   0x0B
#define CIN_PROGRAM_CHANGE   0x0C
#define CIN_CHANNEL_PRESS    0x0D
#define CIN_PITCH_BEND       0x0E
#define CIN_SINGLE_BYTE      0x0F

#define MIDI_NET_MAX_IPMIDI_SOURCES 8
#define MIDI_NET_SYSEX_SCRATCH      (SHADOW_MIDI_INJECT_SLOTS * 3)
#define MIDI_NET_OUTBOUND_SLOTS     256u
#define MIDI_NET_OUTBOUND_MASK      (MIDI_NET_OUTBOUND_SLOTS - 1u)

typedef struct midi_net_stream_parser_t {
    uint8_t running_status;
    uint8_t pending_status;
    uint8_t pending_data[2];
    uint8_t pending_len;
    uint8_t pending_need;
    uint8_t in_sysex;
    uint8_t sysex_overflow;
    uint32_t sysex_len;
    uint8_t sysex_buf[MIDI_NET_SYSEX_SCRATCH];
} midi_net_stream_parser_t;

typedef struct midi_net_ipmidi_source_t {
    int active;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    uint64_t last_activity_ms;
    midi_net_stream_parser_t parser;
} midi_net_ipmidi_source_t;

typedef struct midi_net_outbound_slot_t {
    uint32_t seq;
    uint32_t generation;
    uint8_t pkt[4];
} midi_net_outbound_slot_t;

typedef struct midi_net_state_t {
    shadow_midi_inject_t **inject_shm_ptr;

    pthread_t thread;
    int running;
    int thread_started;
    uint32_t service_generation;
    int self_pipe[2];

    int ipmidi_sock;
    void *rtpmidi_dso;
    schwung_rtpmidi_run_fn rtpmidi_run;

    midi_net_ipmidi_source_t ipmidi_sources[MIDI_NET_MAX_IPMIDI_SOURCES];

    midi_net_outbound_slot_t outbound[MIDI_NET_OUTBOUND_SLOTS];
    uint32_t outbound_enqueue_pos;
    uint32_t outbound_read_pos;

} midi_net_state_t;

extern midi_net_state_t g_midi_net;

void midi_net_log(const char *fmt, ...);
void midi_net_logd(const char *fmt, ...);
uint64_t midi_net_now_ms(void);

int midi_net_inject_usb_packet(uint8_t cin, uint8_t status,
                               uint8_t d1, uint8_t d2);
int midi_net_emit_midi_message(uint8_t status, uint8_t d1, uint8_t d2);
int midi_net_emit_sysex(const uint8_t *bytes, int len);
int midi_net_parse_raw_stream(midi_net_stream_parser_t *parser,
                              const uint8_t *bytes, int len);

int  midi_net_ipmidi_open(void);
void midi_net_ipmidi_close(void);
void midi_net_ipmidi_handle_rx(int sock);

#ifdef MIDI_NET_TESTING
int  midi_net_test_outbound_pop(uint8_t pkt4[4]);
int  midi_net_test_outbound_pop_current(uint8_t pkt4[4]);
#endif

#endif /* MIDI_NET_INTERNAL_H */
