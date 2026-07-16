/* C ABI between the C shim and the optional C++ librtpmidid adapter. */

#ifndef MIDI_NET_RTPMIDI_API_H
#define MIDI_NET_RTPMIDI_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHWUNG_RTPMIDI_ADAPTER_ABI 1u

typedef struct schwung_rtpmidi_config_t {
    uint32_t abi_version;
    const char *session_name;
    uint16_t control_port;
    int stop_fd;
    int ipmidi_fd;
    void *userdata;
    int  (*is_running)(void *userdata);
    void (*handle_ipmidi)(void *userdata, int fd);
    int  (*pop_outbound)(void *userdata, uint8_t packet[4]);
    void (*handle_inbound)(void *userdata, const uint8_t *bytes, size_t len);
    void (*log)(void *userdata, int is_error, const char *message);
} schwung_rtpmidi_config_t;

typedef int (*schwung_rtpmidi_run_fn)(
    const schwung_rtpmidi_config_t *config);

int schwung_rtpmidi_run(const schwung_rtpmidi_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_NET_RTPMIDI_API_H */
