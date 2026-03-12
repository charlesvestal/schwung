/*
 * pw-midi-bridge — Bridge Move's MIDI to ALSA sequencer virtual ports
 *
 * Reads from shared memory ring buffers written by the Move Everything shim
 * and creates two ALSA sequencer ports:
 *   - "Move UI"        (pads, knobs, buttons — MIDI_IN cable 0)
 *   - "Move MIDI Out"  (track output — MIDI_OUT cable 0)
 *
 * PipeWire auto-discovers these as MIDI source nodes.
 *
 * Usage: pw-midi-bridge [-v]
 *   -v  verbose logging to stderr
 *
 * Build (inside chroot or cross-compile):
 *   gcc -O2 -o pw-midi-bridge pw-midi-bridge.c -lasound -lrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <alsa/asoundlib.h>

/* Inline the shared structures (avoid header path dependency) */
#define PW_MIDI_RING_SIZE 256
#define PW_MIDI_RING_MASK (PW_MIDI_RING_SIZE - 1)

typedef struct {
    uint8_t len;
    uint8_t data[3];
} pw_midi_event_t;

typedef struct {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t active;
    uint32_t reserved;
    pw_midi_event_t events[PW_MIDI_RING_SIZE];
} pw_midi_ring_t;

static volatile int running = 1;
static int verbose = 0;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static pw_midi_ring_t *open_shm(const char *name) {
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) return NULL;
    pw_midi_ring_t *ring = mmap(NULL, sizeof(pw_midi_ring_t),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
    close(fd);
    if (ring == MAP_FAILED) return NULL;
    return ring;
}

/* Drain ring buffer, send events to ALSA seq port */
static int drain_ring(pw_midi_ring_t *ring, snd_seq_t *seq, int port) {
    int count = 0;
    while (ring->read_idx != ring->write_idx) {
        __sync_synchronize();
        uint32_t ri = ring->read_idx;
        pw_midi_event_t *ev = &ring->events[ri & PW_MIDI_RING_MASK];
        if (ev->len >= 1 && ev->len <= 3) {
            snd_seq_event_t sev;
            snd_seq_ev_clear(&sev);
            snd_seq_ev_set_source(&sev, port);
            snd_seq_ev_set_subs(&sev);
            snd_seq_ev_set_direct(&sev);

            uint8_t status = ev->data[0] & 0xF0;
            uint8_t ch = ev->data[0] & 0x0F;

            switch (status) {
            case 0x90: /* Note On */
                if (ev->data[2] > 0)
                    snd_seq_ev_set_noteon(&sev, ch, ev->data[1], ev->data[2]);
                else
                    snd_seq_ev_set_noteoff(&sev, ch, ev->data[1], 0);
                break;
            case 0x80: /* Note Off */
                snd_seq_ev_set_noteoff(&sev, ch, ev->data[1], ev->data[2]);
                break;
            case 0xB0: /* CC */
                snd_seq_ev_set_controller(&sev, ch, ev->data[1], ev->data[2]);
                break;
            case 0xE0: /* Pitch Bend */
                {
                    int val = (ev->data[2] << 7) | ev->data[1];
                    snd_seq_ev_set_pitchbend(&sev, ch, val - 8192);
                }
                break;
            case 0xD0: /* Channel Pressure */
                sev.type = SND_SEQ_EVENT_CHANPRESS;
                sev.data.control.channel = ch;
                sev.data.control.value = ev->data[1];
                break;
            case 0xC0: /* Program Change */
                sev.type = SND_SEQ_EVENT_PGMCHANGE;
                sev.data.control.channel = ch;
                sev.data.control.value = ev->data[1];
                break;
            case 0xA0: /* Poly Aftertouch */
                sev.type = SND_SEQ_EVENT_KEYPRESS;
                sev.data.note.channel = ch;
                sev.data.note.note = ev->data[1];
                sev.data.note.velocity = ev->data[2];
                break;
            default:
                goto skip;
            }

            snd_seq_event_output_direct(seq, &sev);
            count++;

            if (verbose && count <= 5) {
                fprintf(stderr, "[midi-bridge] port %d: %02X %02X %02X\n",
                        port, ev->data[0], ev->data[1], ev->data[2]);
            }
skip:;
        }
        ring->read_idx = ri + 1;
    }
    return count;
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) verbose = 1;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Open shared memory */
    pw_midi_ring_t *ring_ui = NULL;
    pw_midi_ring_t *ring_out = NULL;

    fprintf(stderr, "[midi-bridge] Waiting for shm segments...\n");

    /* Retry until shm is available (shim may not have started yet) */
    for (int attempt = 0; attempt < 30 && running; attempt++) {
        if (!ring_ui) ring_ui = open_shm("/move-pw-midi-ui");
        if (!ring_out) ring_out = open_shm("/move-pw-midi-out");
        if (ring_ui && ring_out) break;
        sleep(1);
    }

    if (!ring_ui && !ring_out) {
        fprintf(stderr, "[midi-bridge] ERROR: No shm segments found after 30s\n");
        return 1;
    }

    /* Open ALSA sequencer */
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "[midi-bridge] ERROR: Cannot open ALSA sequencer\n");
        return 1;
    }
    snd_seq_set_client_name(seq, "Move Everything");

    int port_ui = -1, port_out = -1;

    if (ring_ui) {
        port_ui = snd_seq_create_simple_port(seq, "Move UI",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_ui >= 0)
            fprintf(stderr, "[midi-bridge] Created port 'Move UI' (%d)\n", port_ui);
    }

    if (ring_out) {
        port_out = snd_seq_create_simple_port(seq, "Move MIDI Out",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_out >= 0)
            fprintf(stderr, "[midi-bridge] Created port 'Move MIDI Out' (%d)\n", port_out);
    }

    fprintf(stderr, "[midi-bridge] Running (ui=%s, out=%s)\n",
            ring_ui ? "yes" : "no", ring_out ? "yes" : "no");

    /* Main loop: poll ring buffers at ~1ms intervals */
    struct timespec sleep_ts = { 0, 1000000 }; /* 1ms */

    while (running) {
        int activity = 0;
        if (ring_ui && port_ui >= 0)
            activity += drain_ring(ring_ui, seq, port_ui);
        if (ring_out && port_out >= 0)
            activity += drain_ring(ring_out, seq, port_out);

        /* Adaptive sleep: faster when active, slower when idle */
        if (activity > 0) {
            sleep_ts.tv_nsec = 500000;  /* 0.5ms when active */
        } else {
            sleep_ts.tv_nsec = 2000000; /* 2ms when idle */
        }
        nanosleep(&sleep_ts, NULL);
    }

    fprintf(stderr, "[midi-bridge] Shutting down\n");
    snd_seq_close(seq);

    if (ring_ui) munmap(ring_ui, sizeof(pw_midi_ring_t));
    if (ring_out) munmap(ring_out, sizeof(pw_midi_ring_t));

    return 0;
}
