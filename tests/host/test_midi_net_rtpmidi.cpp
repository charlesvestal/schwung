/* Linux integration test for the real librtpmidid adapter. */

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include "midi_net_internal.h"
#include "shadow_midi_inject_writer.h"
}

namespace {

int failures;
#define CHECK(condition, message) do {                                      \
    if (!(condition)) {                                                     \
        std::fprintf(stderr, "FAIL: %s\n", message);                       \
        ++failures;                                                         \
    }                                                                       \
} while (0)

shadow_midi_inject_t injection;
shadow_midi_inject_t *injection_ptr = &injection;

void put_u16(std::vector<uint8_t> &packet, uint16_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 8));
    packet.push_back(static_cast<uint8_t>(value));
}

void put_u32(std::vector<uint8_t> &packet, uint32_t value) {
    packet.push_back(static_cast<uint8_t>(value >> 24));
    packet.push_back(static_cast<uint8_t>(value >> 16));
    packet.push_back(static_cast<uint8_t>(value >> 8));
    packet.push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> command(char a, char b, uint32_t initiator,
                             uint32_t ssrc, bool include_name) {
    std::vector<uint8_t> packet;
    put_u16(packet, 0xffff);
    packet.push_back(static_cast<uint8_t>(a));
    packet.push_back(static_cast<uint8_t>(b));
    put_u32(packet, 2);
    put_u32(packet, initiator);
    put_u32(packet, ssrc);
    if (include_name) {
        static const char name[] = "schwung-test";
        packet.insert(packet.end(), name, name + sizeof(name));
    }
    return packet;
}

std::vector<uint8_t> rtp_packet(uint16_t sequence, uint32_t ssrc,
                                const std::vector<uint8_t> &midi) {
    std::vector<uint8_t> packet = { 0x80, 0x61 };
    put_u16(packet, sequence);
    put_u32(packet, sequence);
    put_u32(packet, ssrc);
    CHECK(midi.size() < 16, "test RTP payload uses the short header");
    packet.push_back(static_cast<uint8_t>(midi.size()));
    packet.insert(packet.end(), midi.begin(), midi.end());
    return packet;
}

bool send_to_server(int fd, int port, const std::vector<uint8_t> &packet) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sendto(fd, packet.data(), packet.size(), 0,
                  reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
           static_cast<ssize_t>(packet.size());
}

int receive_packet(int fd, std::vector<uint8_t> &packet, int timeout_ms) {
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) return 0;
    std::array<uint8_t, 2048> bytes{};
    ssize_t size = recv(fd, bytes.data(), bytes.size(), 0);
    if (size <= 0) return 0;
    packet.assign(bytes.begin(), bytes.begin() + size);
    return 1;
}

bool receive_command(int fd, char a, char b, int timeout_ms,
                     std::vector<uint8_t> *received = nullptr) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    do {
        std::vector<uint8_t> packet;
        if (!receive_packet(fd, packet, 50)) continue;
        if (packet.size() >= 4 && packet[0] == 0xff && packet[1] == 0xff &&
            packet[2] == static_cast<uint8_t>(a) &&
            packet[3] == static_cast<uint8_t>(b)) {
            if (received) *received = std::move(packet);
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool invite(int fd, int server_port, const std::vector<uint8_t> &invitation,
            std::vector<uint8_t> *reply = nullptr) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (!send_to_server(fd, server_port, invitation)) return false;
        if (receive_command(fd, 'O', 'K', 50, reply)) return true;
    }
    return false;
}

bool open_client_pair(int &control, int &midi) {
    for (int port = 42000; port < 52000; port += 2) {
        control = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        midi = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (control < 0 || midi < 0) return false;
        sockaddr_in first{}, second{};
        first.sin_family = second.sin_family = AF_INET;
        first.sin_addr.s_addr = second.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        first.sin_port = htons(static_cast<uint16_t>(port));
        second.sin_port = htons(static_cast<uint16_t>(port + 1));
        if (bind(control, reinterpret_cast<sockaddr *>(&first), sizeof(first)) == 0 &&
            bind(midi, reinterpret_cast<sockaddr *>(&second), sizeof(second)) == 0)
            return true;
        close(control);
        close(midi);
        control = midi = -1;
    }
    return false;
}

bool pop_injected(std::array<uint8_t, 4> &packet, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    do {
        if (shadow_midi_inject_peek(&injection, packet.data())) {
            shadow_midi_inject_pop(&injection);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool receive_outbound_note(int fd, uint8_t note, uint8_t velocity) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
    do {
        std::vector<uint8_t> packet;
        if (!receive_packet(fd, packet, 50)) continue;
        if (packet.size() >= 16 && packet[0] == 0x80 &&
            (packet[1] & 0x7f) == 0x61 && packet[12] == 3 &&
            packet[13] == 0x90 && packet[14] == note &&
            packet[15] == velocity)
            return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void drain_socket(int fd) {
    pollfd pfd{fd, POLLIN, 0};
    std::array<uint8_t, 2048> bytes{};
    while (poll(&pfd, 1, 0) > 0)
        (void)recv(fd, bytes.data(), bytes.size(), 0);
}

} // namespace

int main() {
    constexpr uint32_t initiator = 0x12345678;
    constexpr uint32_t client_ssrc = 0x00beef00;
    int control = -1, midi = -1;
    CHECK(open_client_pair(control, midi), "open consecutive UDP client ports");
    if (control < 0 || midi < 0) return 1;

    shadow_midi_inject_init(&injection);
    midi_net_init(&injection_ptr);
    g_midi_net.rtpmidi_run = schwung_rtpmidi_run;
    midi_net_reconcile(1);

    const auto invitation = command('I', 'N', initiator, client_ssrc, true);
    std::vector<uint8_t> control_reply;
    CHECK(invite(control, 5004, invitation, &control_reply),
          "negotiate AppleMIDI control port");
    CHECK(control_reply.size() >= 16, "control response includes server SSRC");
    CHECK(invite(midi, 5005, invitation), "negotiate AppleMIDI MIDI port");

    auto note = rtp_packet(1, client_ssrc, { 0x90, 60, 100 });
    CHECK(send_to_server(midi, 5005, note), "send inbound RTP note");
    std::array<uint8_t, 4> usb{};
    CHECK(pop_injected(usb, 1000) && usb[0] == 0x39 && usb[1] == 0x90 &&
          usb[2] == 60 && usb[3] == 100,
          "inbound RTP note is converted to injected USB-MIDI");

    auto sysex = rtp_packet(2, client_ssrc, { 0xf0, 1, 2, 3, 0xf7 });
    CHECK(send_to_server(midi, 5005, sysex), "send inbound RTP SysEx");
    CHECK(pop_injected(usb, 1000) && usb[0] == 0x34 && usb[1] == 0xf0 &&
          usb[2] == 1 && usb[3] == 2,
          "inbound RTP SysEx injects its first USB-MIDI packet");
    CHECK(pop_injected(usb, 1000) && usb[0] == 0x36 && usb[1] == 3 &&
          usb[2] == 0xf7,
          "inbound RTP SysEx injects its final USB-MIDI packet");

    const uint8_t outbound[4] = { 0x29, 0x90, 67, 111 };
    midi_net_publish(outbound);
    CHECK(receive_outbound_note(midi, 67, 111),
          "realtime outbound note is delivered as RTP-MIDI");

    const auto goodbye = command('B', 'Y', initiator, client_ssrc, false);
    CHECK(send_to_server(control, 5004, goodbye), "disconnect control session");
    CHECK(send_to_server(midi, 5005, goodbye), "disconnect MIDI session");
    midi_net_reconcile(0);
    drain_socket(control);
    drain_socket(midi);

    midi_net_reconcile(1);
    CHECK(invite(control, 5004, invitation),
          "reconnect control port after service restart");
    CHECK(invite(midi, 5005, invitation),
          "reconnect MIDI port after service restart");
    midi_net_reconcile(0);

    close(control);
    close(midi);
    if (failures) {
        std::fprintf(stderr, "%d real-adapter integration test(s) failed\n",
                     failures);
        return 1;
    }
    std::puts("real RTP-MIDI adapter integration tests passed");
    return 0;
}
