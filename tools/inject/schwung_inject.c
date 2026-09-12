/* schwung_inject - push USB-MIDI packets into Move's MIDI_IN, as if a control
 * had been pressed. The device-side half of a hands-free hardware test.
 *
 * WHY A BINARY AND NOT A SCRIPT. /schwung-midi-inject is Vyukov's bounded
 * MPSC ring and the packets land in Move's own mailbox: a cable-0/CIN-0 packet
 * reaching MIDI_IN reads as "misc function" and Move's firmware ABORTS. So the
 * push goes through shadow_midi_inject_writer.h -- the one implementation, with
 * the real atomics and the release/acquire pairing -- rather than a hand-rolled
 * copy of the protocol in whatever language was to hand.
 *
 * Outside overtake mode the shim drains this ring into the mailbox, which is
 * why an injected packet is indistinguishable from a hardware press. In
 * overtake mode it is diverted to the module instead (schwung_shim.c), so this
 * tool addresses MOVE, and only while no overtake module is up.
 *
 * usage: schwung_inject <hdr> <status> <d1> <d2> [...]        (hex or decimal)
 *        schwung_inject step <1-16> [gap_ms]     press and release a step
 *        schwung_inject track <1-4> [gap_ms]     press and release a track
 *
 * Steps are notes 16..31 and tracks CCs 40..43 REVERSED (CC43 = Track 1);
 * both conventions are Move's, restated in CLAUDE.md's hardware section.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "shadow_constants.h"
#include "shadow_midi_inject_writer.h"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <hdr> <status> <d1> <d2> [...]\n"
                        "       %s step <1-16> [gap_ms]\n"
                        "       %s track <1-4> [gap_ms]\n", argv[0], argv[0], argv[0]);
        return 2;
    }

    /* The shim CREATES this segment and initialises the slot sequences; a
     * producer must never create it, because a zero-filled ring is not a valid
     * initial state (seq[i] must equal i) and the consumer would read garbage
     * out of it. O_RDWR only, and a missing segment is "the shim is not
     * running", which is worth saying plainly. */
    int fd = shm_open(SHM_SHADOW_MIDI_INJECT, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "%s not present -- is the shim running?\n",
                SHM_SHADOW_MIDI_INJECT);
        return 1;
    }
    void *map = mmap(NULL, sizeof(shadow_midi_inject_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    shadow_midi_inject_t *ring = (shadow_midi_inject_t *)map;

    uint8_t pkts[64][4];
    int n = 0;
    int gap = 60;

    if (strcmp(argv[1], "step") == 0 || strcmp(argv[1], "track") == 0) {
        if (argc < 3) { fprintf(stderr, "need a number\n"); return 2; }
        long which = strtol(argv[2], NULL, 10);
        if (argc > 3) gap = (int)strtol(argv[3], NULL, 10);
        if (strcmp(argv[1], "step") == 0) {
            if (which < 1 || which > 16) { fprintf(stderr, "step 1-16\n"); return 2; }
            uint8_t note = (uint8_t)(16 + which - 1);
            pkts[n][0] = 0x09; pkts[n][1] = 0x90; pkts[n][2] = note; pkts[n][3] = 127; n++;
            pkts[n][0] = 0x08; pkts[n][1] = 0x80; pkts[n][2] = note; pkts[n][3] = 0;   n++;
        } else {
            if (which < 1 || which > 4) { fprintf(stderr, "track 1-4\n"); return 2; }
            /* CC43 = Track 1 ... CC40 = Track 4. */
            uint8_t cc = (uint8_t)(43 - (which - 1));
            pkts[n][0] = 0x0B; pkts[n][1] = 0xB0; pkts[n][2] = cc; pkts[n][3] = 127; n++;
            pkts[n][0] = 0x0B; pkts[n][1] = 0xB0; pkts[n][2] = cc; pkts[n][3] = 0;   n++;
        }
    } else {
        if ((argc - 1) % 4 != 0) {
            fprintf(stderr, "raw packets come in fours (hdr status d1 d2)\n");
            return 2;
        }
        for (int i = 1; i < argc && n < 64; i += 4, n++)
            for (int b = 0; b < 4; b++)
                pkts[n][b] = (uint8_t)strtol(argv[i + b], NULL, 0);
    }

    for (int i = 0; i < n; i++) {
        /* A packet whose header says cable 0 AND code-index 0 is the one shape
         * that aborts Move's firmware. Refuse it here rather than push it: a
         * typo on a command line should not take the device down. */
        if (pkts[i][0] == 0x00) {
            fprintf(stderr, "refusing packet %d: hdr 0x00 is cable 0 / CIN 0, "
                            "which aborts Move's firmware\n", i);
            return 2;
        }
        /* 0 is SUCCESS and -1 is a full ring -- not a boolean. Testing it as
         * one reported "ring full" on every successful push and success on a
         * real drop, which made a working injection look broken and would
         * have made a silently dropped packet look delivered. */
        if (shadow_midi_inject_push(ring, pkts[i]) != 0) {
            fprintf(stderr, "ring full at packet %d -- is the shim draining?\n", i);
            return 1;
        }
        printf("pushed %02x %02x %02x %02x\n",
               pkts[i][0], pkts[i][1], pkts[i][2], pkts[i][3]);
        if (i + 1 < n) nap_ms(gap);
    }
    return 0;
}
