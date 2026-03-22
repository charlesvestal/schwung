/*
 * midi_inject_test.c - Standalone test tool for MIDI injection into Move
 *
 * Opens /move-shadow-midi-inject SHM and writes USB-MIDI packets that the
 * shim drains into Move's MIDI_IN buffer, making Move process them as
 * real hardware events.
 *
 * Usage:
 *   midi_inject_test pad <note> <velocity>  - note-on + 100ms + note-off
 *   midi_inject_test play                   - CC 85 toggle (play/stop)
 *   midi_inject_test record                 - CC 86 toggle
 *   midi_inject_test scene <col>            - launch 4 pads in column (0-7)
 *   midi_inject_test stop                   - MIDI Stop (0xFC, CIN 0x0F)
 *   midi_inject_test start                  - MIDI Start (0xFA, CIN 0x0F)
 *   midi_inject_test select <n1> <n2> ...   - note-ons only (no note-off), for testing selection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "host/shadow_constants.h"

static shadow_midi_inject_t *inject_shm = NULL;

static int open_inject_shm(void)
{
    int fd = shm_open(SHM_SHADOW_MIDI_INJECT, O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        fprintf(stderr, "Is Schwung running?\n");
        return -1;
    }
    inject_shm = (shadow_midi_inject_t *)mmap(NULL, sizeof(shadow_midi_inject_t),
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fd, 0);
    close(fd);
    if (inject_shm == MAP_FAILED) {
        perror("mmap");
        inject_shm = NULL;
        return -1;
    }
    return 0;
}

/* Write a single USB-MIDI packet: [CIN|cable, status, d1, d2] */
static void write_packet(uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2)
{
    int idx = inject_shm->write_idx;
    if (idx + 4 > SHADOW_MIDI_INJECT_BUFFER_SIZE) {
        fprintf(stderr, "Buffer full\n");
        return;
    }
    inject_shm->buffer[idx]     = cin;       /* CIN nibble, cable 0 */
    inject_shm->buffer[idx + 1] = status;
    inject_shm->buffer[idx + 2] = d1;
    inject_shm->buffer[idx + 3] = d2;
    inject_shm->write_idx = idx + 4;
}

/* Signal the shim that data is ready */
static void flush(void)
{
    __sync_synchronize();
    inject_shm->ready++;
}

/* Send note-on, wait, send note-off */
static void cmd_pad(int note, int velocity)
{
    printf("Pad: note %d velocity %d\n", note, velocity);

    /* Note-on: CIN=0x09, status=0x90 (ch1), note, vel */
    write_packet(0x09, 0x90, (uint8_t)note, (uint8_t)velocity);
    flush();

    usleep(100000);  /* 100ms */

    /* Note-off: CIN=0x08, status=0x80 (ch1), note, 0 */
    write_packet(0x08, 0x80, (uint8_t)note, 0);
    flush();
}

/* Send CC on/off toggle */
static void cmd_cc_toggle(int cc, const char *name)
{
    printf("%s: CC %d on\n", name, cc);
    write_packet(0x0B, 0xB0, (uint8_t)cc, 127);
    flush();

    usleep(100000);

    printf("%s: CC %d off\n", name, cc);
    write_packet(0x0B, 0xB0, (uint8_t)cc, 0);
    flush();
}

/* Launch a scene column: 4 simultaneous pad note-ons */
static void cmd_scene(int col)
{
    if (col < 0 || col > 7) {
        fprintf(stderr, "Column must be 0-7\n");
        return;
    }

    /* Track 1-4 notes for this column */
    int notes[4] = { 92 + col, 84 + col, 76 + col, 68 + col };

    printf("Scene col %d: notes %d %d %d %d\n", col,
           notes[0], notes[1], notes[2], notes[3]);

    /* Send one note-on per ioctl tick — only ~1 empty MIDI_IN slot per tick */
    for (int i = 0; i < 4; i++) {
        write_packet(0x09, 0x90, (uint8_t)notes[i], 100);
        flush();
        usleep(5000);  /* 5ms — one ioctl tick is ~2.9ms */
    }

    usleep(100000);

    /* Note-offs, same pattern */
    for (int i = 0; i < 4; i++) {
        write_packet(0x08, 0x80, (uint8_t)notes[i], 0);
        flush();
        usleep(5000);
    }
}

/* Send MIDI System Real-Time: Stop (0xFC) */
static void cmd_stop(void)
{
    printf("MIDI Stop (0xFC, CIN 0x0F)\n");
    write_packet(0x0F, 0xFC, 0x00, 0x00);
    flush();
}

/* Send MIDI System Real-Time: Start (0xFA) */
static void cmd_start(void)
{
    printf("MIDI Start (0xFA, CIN 0x0F)\n");
    write_packet(0x0F, 0xFA, 0x00, 0x00);
    flush();
}

/* Parse a column letter (A-H) and select all 4 tracks in that column */
static int parse_column_letter(const char *arg)
{
    if (strlen(arg) == 1) {
        char c = arg[0];
        if (c >= 'A' && c <= 'H') return c - 'A';
        if (c >= 'a' && c <= 'h') return c - 'a';
    }
    return -1;
}

static void select_note(int note)
{
    printf("Select: note %d\n", note);
    write_packet(0x09, 0x90, (uint8_t)note, 100);
    flush();
    usleep(50000);  /* 50ms hold */
    write_packet(0x08, 0x80, (uint8_t)note, 0);
    flush();
    usleep(5000);  /* 5ms gap before next */
}

/* Send note-on + delay + note-off for pad notes or column letters (A-H) */
static void cmd_select(int argc, char *argv[])
{
    for (int i = 2; i < argc; i++) {
        int col = parse_column_letter(argv[i]);
        if (col >= 0) {
            /* Column letter: select all 4 tracks */
            int notes[4] = { 92 + col, 84 + col, 76 + col, 68 + col };
            printf("Column %c: notes %d %d %d %d\n", 'A' + col,
                   notes[0], notes[1], notes[2], notes[3]);
            for (int t = 0; t < 4; t++) select_note(notes[t]);
        } else {
            /* Numeric note */
            select_note(atoi(argv[i]));
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s pad <note> <velocity>  - trigger pad\n", prog);
    fprintf(stderr, "  %s play                   - toggle play (CC 85)\n", prog);
    fprintf(stderr, "  %s record                 - toggle record (CC 86)\n", prog);
    fprintf(stderr, "  %s scene <col>            - launch scene column (0-7)\n", prog);
    fprintf(stderr, "  %s stop                   - MIDI Stop (0xFC)\n", prog);
    fprintf(stderr, "  %s start                  - MIDI Start (0xFA)\n", prog);
    fprintf(stderr, "  %s select <n1|A-H> ...    - select pads (notes or column letters)\n", prog);
    fprintf(stderr, "\nPad grid:\n");
    fprintf(stderr, "  92-99: Track 1 (top)    84-91: Track 2\n");
    fprintf(stderr, "  76-83: Track 3          68-75: Track 4 (bottom)\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (open_inject_shm() != 0) {
        return 1;
    }

    if (strcmp(argv[1], "pad") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s pad <note> <velocity>\n", argv[0]);
            return 1;
        }
        cmd_pad(atoi(argv[2]), atoi(argv[3]));
    } else if (strcmp(argv[1], "play") == 0) {
        cmd_cc_toggle(85, "Play");
    } else if (strcmp(argv[1], "record") == 0) {
        cmd_cc_toggle(86, "Record");
    } else if (strcmp(argv[1], "scene") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s scene <col>\n", argv[0]);
            return 1;
        }
        cmd_scene(atoi(argv[2]));
    } else if (strcmp(argv[1], "stop") == 0) {
        cmd_stop();
    } else if (strcmp(argv[1], "start") == 0) {
        cmd_start();
    } else if (strcmp(argv[1], "select") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s select <note1> [note2] ...\n", argv[0]);
            return 1;
        }
        cmd_select(argc, argv);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(argv[0]);
        return 1;
    }

    return 0;
}
