/* midi_net_internal.h - internal state shared by network MIDI backends. */

#ifndef MIDI_NET_INTERNAL_H
#define MIDI_NET_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>

#include "midi_net.h"

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

#define MIDI_NET_MAX_PEERS          4
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

typedef struct midi_net_peer_t {
    int active; /* 1=control accepted, 2=data accepted, 3=established */
    uint32_t initiator_token;
    uint32_t ssrc;
    uint32_t peer_ssrc;
    uint32_t ck_count;
    struct sockaddr_storage ctrl_addr;
    socklen_t ctrl_addrlen;
    struct sockaddr_storage evt_addr;
    socklen_t evt_addrlen;
    uint8_t sysex_buf[MIDI_NET_SYSEX_SCRATCH];
    uint32_t sysex_len;
    uint8_t sysex_overflow;
    uint8_t last_status;
    uint64_t last_activity_ms;
} midi_net_peer_t;

typedef struct midi_net_outbound_slot_t {
    uint32_t seq;
    uint32_t generation;
    uint8_t pkt[4];
} midi_net_outbound_slot_t;

typedef struct midi_net_state_t {
    uint32_t backend_flags;
    shadow_midi_inject_t **inject_shm_ptr;
    void (*log_fn)(const char *msg);

    pthread_t thread;
    int running;
    int thread_started;
    uint32_t service_generation;
    int self_pipe[2];

    int ipmidi_sock;
    int applemidi_ctrl_sock;
    int applemidi_evt_sock;
    int mdns_sock;

    midi_net_ipmidi_source_t ipmidi_sources[MIDI_NET_MAX_IPMIDI_SOURCES];
    midi_net_peer_t peers[MIDI_NET_MAX_PEERS];

    midi_net_stats_t stats;

    midi_net_outbound_slot_t outbound[MIDI_NET_OUTBOUND_SLOTS];
    uint32_t outbound_enqueue_pos;
    uint32_t outbound_read_pos;

    uint16_t applemidi_seq;
    uint32_t our_ssrc;
} midi_net_state_t;

extern midi_net_state_t g_midi_net;

void midi_net_log(const char *fmt, ...);
void midi_net_logd(const char *fmt, ...);
uint64_t midi_net_now_ms(void);
uint64_t midi_net_now_100us(void);

void midi_net_stat_inc_u64(uint64_t *field);
void midi_net_stat_store_u32(uint32_t *field, uint32_t value);

int midi_net_inject_usb_packet(uint8_t cin, uint8_t status,
                               uint8_t d1, uint8_t d2);
int midi_net_emit_midi_message(uint8_t status, uint8_t d1, uint8_t d2);
int midi_net_emit_sysex(const uint8_t *bytes, int len);
int midi_net_parse_raw_stream(midi_net_stream_parser_t *parser,
                              const uint8_t *bytes, int len);

int  midi_net_ipmidi_open(void);
void midi_net_ipmidi_close(void);
void midi_net_ipmidi_handle_rx(int sock);

int  midi_net_applemidi_open(void);
void midi_net_applemidi_close(void);
void midi_net_applemidi_handle_ctrl(int sock);
void midi_net_applemidi_handle_evt(int sock);
void midi_net_applemidi_tick(void);
void midi_net_applemidi_send_midi(const uint8_t *pkt4);

int  midi_net_mdns_open(void);
void midi_net_mdns_close(void);
void midi_net_mdns_handle_rx(int sock);
void midi_net_mdns_announce(void);

#ifdef MIDI_NET_TESTING
int  midi_net_test_outbound_pop(uint8_t pkt4[4]);
int  midi_net_test_outbound_pop_current(uint8_t pkt4[4]);
void midi_net_test_parse_rtp(midi_net_peer_t *peer,
                             const uint8_t *packet, int len);
int  midi_net_test_decode_dns_name(const uint8_t *packet, int packet_len,
                                   int offset, char *out, int out_len);
#endif

#endif /* MIDI_NET_INTERNAL_H */
