/* Optional librtpmidid adapter. All C++ and third-party state stays here. */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unistd.h>

#include <rtpmidid/iobytes.hpp>
#include <rtpmidid/mdns_rtpmidi.hpp>
#include <rtpmidid/poller.hpp>
#include <rtpmidid/rtpserver.hpp>

#include "midi_net_rtpmidi_api.h"

namespace {

enum : uint8_t {
    CIN_SYSTEM_COMMON_2 = 0x02,
    CIN_SYSTEM_COMMON_3 = 0x03,
    CIN_NOTE_OFF = 0x08,
    CIN_NOTE_ON = 0x09,
    CIN_POLY_KEY_PRESS = 0x0a,
    CIN_CONTROL_CHANGE = 0x0b,
    CIN_PROGRAM_CHANGE = 0x0c,
    CIN_CHANNEL_PRESS = 0x0d,
    CIN_PITCH_BEND = 0x0e,
    CIN_SINGLE_BYTE = 0x0f,
};

void log_message(const schwung_rtpmidi_config_t *config, bool error,
                 const char *message) {
    if (config->log) config->log(config->userdata, error ? 1 : 0, message);
}

int usb_packet_to_midi(const uint8_t packet[4], uint8_t bytes[3]) {
    const uint8_t cin = packet[0] & 0x0f;
    bytes[0] = packet[1];
    switch (cin) {
    case CIN_NOTE_OFF:
    case CIN_NOTE_ON:
    case CIN_POLY_KEY_PRESS:
    case CIN_CONTROL_CHANGE:
    case CIN_PITCH_BEND:
    case CIN_SYSTEM_COMMON_3:
        bytes[1] = packet[2];
        bytes[2] = packet[3];
        return 3;
    case CIN_PROGRAM_CHANGE:
    case CIN_CHANNEL_PRESS:
    case CIN_SYSTEM_COMMON_2:
        bytes[1] = packet[2];
        return 2;
    case CIN_SINGLE_BYTE:
        return 1;
    default:
        return 0; /* Outbound SysEx remains intentionally unsupported. */
    }
}

void drain_fd(int fd) {
    char bytes[32];
    while (read(fd, bytes, sizeof(bytes)) > 0) { }
}

} // namespace

extern "C" int schwung_rtpmidi_run(
    const schwung_rtpmidi_config_t *config) {
    if (!config || config->abi_version != SCHWUNG_RTPMIDI_ADAPTER_ABI ||
        !config->session_name || !config->is_running ||
        !config->pop_outbound || !config->handle_inbound) {
        return -1;
    }

    try {
        rtpmidid::rtpserver_t server(
            config->session_name, std::to_string(config->control_port));
        auto midi_connection = server.midi_event.connect(
            [config](const rtpmidid::io_bytes_reader &data) {
                config->handle_inbound(config->userdata, data.start,
                                       data.size());
            });

        std::unique_ptr<rtpmidid::mdns_rtpmidi_t> mdns;
        try {
            mdns = std::make_unique<rtpmidid::mdns_rtpmidi_t>();
        } catch (const std::exception &error) {
            std::string message = "Bonjour discovery unavailable: ";
            message += error.what();
            log_message(config, true, message.c_str());
        } catch (...) {
            log_message(config, true, "Bonjour discovery unavailable");
        }
        bool announced = false;
        bool stop_requested = false;

        rtpmidid::poller_t::listener_t stop_listener;
        if (config->stop_fd >= 0) {
            stop_listener = rtpmidid::poller.add_fd_in(
                config->stop_fd, [&stop_requested](int fd) {
                    drain_fd(fd);
                    stop_requested = true;
                });
        }

        rtpmidid::poller_t::listener_t ipmidi_listener;
        if (config->ipmidi_fd >= 0 && config->handle_ipmidi) {
            ipmidi_listener = rtpmidid::poller.add_fd_in(
                config->ipmidi_fd, [config](int fd) {
                    config->handle_ipmidi(config->userdata, fd);
                });
        }

        log_message(config, false, "RTP-MIDI server listening on ports 5004/5005");

        while (!stop_requested && config->is_running(config->userdata)) {
            rtpmidid::poller.wait(std::chrono::milliseconds(5));

            /* avahi_client_new may complete asynchronously. Publish once the
             * library has created its entry group. */
            if (mdns && !announced && mdns->group) {
                try {
                    mdns->announce_rtpmidi(config->session_name, server.port());
                    announced = true;
                } catch (const std::exception &error) {
                    std::string message = "Bonjour publication failed: ";
                    message += error.what();
                    log_message(config, true, message.c_str());
                    mdns.reset();
                } catch (...) {
                    log_message(config, true, "Bonjour publication failed");
                    mdns.reset();
                }
            }

            uint8_t packet[4];
            int drained = 0;
            while (drained++ < 256 &&
                   config->pop_outbound(config->userdata, packet)) {
                uint8_t midi[3];
                int len = usb_packet_to_midi(packet, midi);
                if (!len) continue;
                rtpmidid::io_bytes_reader message(midi, static_cast<size_t>(len));
                server.send_midi_to_all_peers(message);
            }
        }

        /* Remove epoll registrations before the C owner closes these fds. */
        ipmidi_listener.stop();
        stop_listener.stop();
        if (announced && mdns) {
            try {
                mdns->unannounce_rtpmidi(config->session_name, server.port());
            } catch (const std::exception &error) {
                std::string message = "Bonjour cleanup failed: ";
                message += error.what();
                log_message(config, true, message.c_str());
            } catch (...) {
                log_message(config, true, "Bonjour cleanup failed");
            }
        }
        return 0;
    } catch (const std::exception &error) {
        log_message(config, true, error.what());
    } catch (...) {
        log_message(config, true, "unknown exception in RTP-MIDI adapter");
    }
    return -1;
}
