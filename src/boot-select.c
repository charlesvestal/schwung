// boot-select.c — SPI boot window + picker binary.
//
// Invoked by /opt/move/Move (the boot selector shell script) BEFORE anything
// else owns /dev/ablspi0.0. Two modes:
//
//   boot-select --window <id>   paint "Loading <name>" + "press Back to
//                                change" for ~2s; a Back press-edge opens the
//                                picker; a timeout prints <id> and exits 0.
//   boot-select --forced <id>   skip the window, open the picker immediately
//                                with a "<name> failed to start" banner and
//                                the cursor on the stock row.
//
// Picker: jog scrolls one row per accumulated detent, jog click writes the
// chosen id to $BOOT_TARGETS_DIR/default (atomically: write a .default.tmp
// then rename() over default) and prints it; Back cancels and prints the
// incoming id unchanged.
//
// stdout carries EXACTLY one line — the chosen/incoming id — on every
// success path. A hard failure (SPI open/mmap, no rows at all) writes
// nothing to stdout, exits 1, and puts a diagnostic on stderr. A SIGALRM
// after 60s is the wedge backstop: it writes the incoming id + newline with
// write() and _exit()s, never touching the display or unmapping anything
// (async-signal-safety), so a stuck picker can never hold boot forever.
//
// All the press-edge / jog-decode / JSON-field / row-model logic this file
// drives lives in host/boot_select_core.{h,c} and is unit-tested there on
// the host — this file is the thin SPI + display glue around it.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* sys/ioctl.h on Linux (glibc) defines _IOC()/_IOC_NONE the way the ablspi
 * driver's custom command numbers are built here. macOS's sys/ioctl.h does
 * not, so `cc -fsyntax-only` on the host (this binary never runs there —
 * see CLAUDE.md, "Shadow mode only") needs a stand-in with the same shape
 * just so this file's OWN logic can be checked. Cross-compiling for Move
 * picks up the real glibc macros untouched. */
#ifndef _IOC_NONE
#define _IOC_NONE 0
#endif
#ifndef _IOC
static inline unsigned bs_iocbuild(unsigned dir, unsigned type, unsigned nr, unsigned size) {
    return (dir << 30) | (type << 8) | nr | (size << 16);
}
#define _IOC(dir, type, nr, size) bs_iocbuild((unsigned)(dir), (unsigned)(type), (unsigned)(nr), (unsigned)(size))
#endif

#include "host/boot_select_core.h"
#include "host/js_display.h"
#include "lib/schwung_spi_lib.h"

#define BS_MAX_ROWS       16
#define BS_WINDOW_FRAMES  700   /* ~2s at ~2.9ms/frame */
#define BS_ALARM_SECONDS  60
#define BS_VISIBLE_ROWS   4   /* rows at y=20+9i; a 5th would collide with the footer */

static char g_incoming_id[64];

/* Async-signal-safe: write() + _exit() only. No display cleanup, no munmap —
 * a wedged picker must never hold boot forever, and the handler must never
 * touch anything that could itself block or allocate. */
static void bs_alarm_handler(int signum) {
    (void)signum;
    size_t len = strlen(g_incoming_id);
    if (len > 0)
        write(STDOUT_FILENO, g_incoming_id, len);
    write(STDOUT_FILENO, "\n", 1);
    _exit(0);
}

static void bs_die(const char *what) {
    fprintf(stderr, "boot-select: %s: %s\n", what, strerror(errno));
    exit(1);
}

static int bs_find_row(const bs_row_t *rows, int nrows, const char *id) {
    for (int i = 0; i < nrows; i++) {
        if (strcmp(rows[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* Writes the chosen id atomically to $BOOT_TARGETS_DIR/default. */
static int bs_write_default(const char *dir, const char *id) {
    char tmp_path[600];
    char final_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.default.tmp", dir);
    snprintf(final_path, sizeof(final_path), "%s/default", dir);

    FILE *f = fopen(tmp_path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", id);
    if (fflush(f) != 0) {
        fclose(f);
        return 0;
    }
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        return 0;
    }
    if (fclose(f) != 0)
        return 0;

    if (rename(tmp_path, final_path) != 0)
        return 0;
    return 1;
}

/* Current frame, in the 1024-byte packed form the SPI protocol carries.
 * Draw into js_display's buffer, then bs_repaint() to publish it here. */
static uint8_t g_packed[SCHWUNG_DISPLAY_SIZE];

static void bs_repaint(void) {
    js_display_pack(g_packed);
}

/* The display is a PULL protocol (docs/SPI_PROTOCOL.md, "Index handshake"):
 * the XMOS requests slice N by putting N in the RX display status word, and
 * the writer echoes N in the TX status word alongside that chunk. Unsolicited
 * pushes are ignored — a blind 6-slice push drew nothing at cold boot, which
 * is how the boot window shipped invisible; it only ever appeared to work
 * after a running MoveOriginal had the handshake mid-cycle. The staged TX
 * response goes out with the NEXT transfer, so call this every frame right
 * after the pump and the request is answered one frame later. */
static void bs_serve_display(uint8_t *map) {
    uint32_t idx;
    memcpy(&idx, map + SCHWUNG_OFF_IN_DISP_STAT, sizeof(idx));
    if (idx < 1 || idx > 6)
        return;
    int off = (int)(idx - 1) * SCHWUNG_OUT_DISP_CHUNK_LEN;
    int len = (idx == 6) ? (SCHWUNG_DISPLAY_SIZE - off) : SCHWUNG_OUT_DISP_CHUNK_LEN;
    memcpy(map + SCHWUNG_OFF_OUT_DISP_STAT, &idx, sizeof(idx));
    memcpy(map + SCHWUNG_OFF_OUT_DISP_DATA, g_packed + off, len);
}

/* Pumps one SPI frame: blocks until the ~2.9ms transfer IRQ, so the input
 * loops below need no extra sleep of their own. */
static void bs_pump_frame(int fd) {
    ioctl(fd, _IOC(_IOC_NONE, 0, SCHWUNG_IOCTL_WAIT_SEND_SIZE, 0), 0x300);
}

/* ── Boot LED-animation cancel ────────────────────────────────────────────
 * The XMOS runs the power-on LED sweep as per-LED ANIMATIONS: colors live on
 * MIDI channel 1, animations on channel 2, and an animation persists until
 * something sends anim-none (0x00) for that address on channel 2. Move's
 * firmware does that at startup — a third-party boot target does not know
 * to, so the sweep ran forever under one (observed on hardware: repainting
 * colors did not stop it). boot-select owns the device during the window,
 * so it cancels every animation once, and every target inherits a still
 * surface. Address table from schwung-spi's schwung_move_ui.c (MIT).
 * {is_cc, addr}: notes are pads 68-99 + steps 16-31; the rest are CCs. */
typedef struct { uint8_t is_cc; uint8_t addr; } bs_led_t;
static const bs_led_t bs_all_leds[] = {
    {0,68},{0,69},{0,70},{0,71},{0,72},{0,73},{0,74},{0,75},
    {0,76},{0,77},{0,78},{0,79},{0,80},{0,81},{0,82},{0,83},
    {0,84},{0,85},{0,86},{0,87},{0,88},{0,89},{0,90},{0,91},
    {0,92},{0,93},{0,94},{0,95},{0,96},{0,97},{0,98},{0,99},
    {0,16},{0,17},{0,18},{0,19},{0,20},{0,21},{0,22},{0,23},
    {0,24},{0,25},{0,26},{0,27},{0,28},{0,29},{0,30},{0,31},
    {1,16},{1,17},{1,18},{1,19},{1,20},{1,21},{1,22},{1,23},
    {1,24},{1,25},{1,26},{1,27},{1,28},{1,29},{1,30},{1,31},
    {1,40},{1,41},{1,42},{1,43},
    {1,71},{1,72},{1,73},{1,74},{1,75},{1,76},{1,77},{1,78},
    {1,85},{1,86},{1,118},
    {1,49},{1,50},{1,51},{1,52},{1,54},{1,55},{1,56},
    {1,58},{1,60},{1,62},{1,63},{1,88},{1,119},
};

/* MIDI_OUT is 20 x 4-byte slots at TX offset 0, consumed per transfer.
 * Emit anim-none on channel 2 (0-indexed 1) for every LED, 20 per frame. */
static void bs_cancel_led_anims(int fd, uint8_t *map) {
    const int total = (int)(sizeof(bs_all_leds) / sizeof(bs_all_leds[0]));
    int sent = 0;
    while (sent < total) {
        int batch = total - sent;
        if (batch > 20)
            batch = 20;
        for (int i = 0; i < batch; i++) {
            const bs_led_t *led = &bs_all_leds[sent + i];
            uint8_t *slot = map + i * 4;
            uint8_t status = (uint8_t)((led->is_cc ? 0xB0 : 0x90) | 0x01);
            slot[0] = (uint8_t)(led->is_cc ? 0x0B : 0x09);  /* cable 0 | CIN */
            slot[1] = status;
            slot[2] = led->addr;
            slot[3] = 0x00;                                  /* ANIM_NONE */
        }
        bs_pump_frame(fd);
        memset(map, 0, 20 * 4);
        sent += batch;
    }
}

/* MIDI_IN events persist in the RX mailbox across frames until overwritten
 * (the shim keeps dedup rings for exactly this reason). boot-select is the
 * device's exclusive owner here, so the simplest fix is to scan once, then
 * zero the scanned region ourselves — otherwise a jog detent or a press
 * read on frame N is replayed and double-counted on frame N+1. */
static void bs_scan_input(uint8_t *map, bs_input_state_t *st,
                           bs_input_events_t *ev) {
    bs_input_scan(st, map + SCHWUNG_OFF_IN_MIDI, ev);
    memset(map + SCHWUNG_OFF_IN_MIDI, 0, BS_MIDI_IN_BYTES);
}

static void bs_print_centered(int y, const char *s) {
    int w = js_display_text_width(s);
    int x = (DISPLAY_WIDTH - w) / 2;
    if (x < 0)
        x = 0;
    js_display_print(x, y, s, 1);
}

static void bs_draw_window(const char *name) {
    js_display_clear();
    char line1[96];
    snprintf(line1, sizeof(line1), "Loading %s", name);
    bs_print_centered(20, line1);
    bs_print_centered(40, "press Back to change");
}

/* Two banner lines: a target name plus "failed to start" is wider than the
 * 128px panel in this font (verified on hardware — it clipped mid-word), so
 * the forced banner puts the name on line 1 and the verdict on line 2. The
 * normal picker leaves line 2 empty. Rows keep their position either way so
 * the two picker variants do not jump. */
static void bs_draw_picker(const char *banner1, const char *banner2,
                            const bs_row_t *rows, int nrows,
                            int cursor, int *scroll) {
    if (cursor < *scroll)
        *scroll = cursor;
    if (cursor >= *scroll + BS_VISIBLE_ROWS)
        *scroll = cursor - (BS_VISIBLE_ROWS - 1);
    if (*scroll < 0)
        *scroll = 0;

    js_display_clear();
    js_display_print(0, 0, banner1, 1);
    if (banner2 && banner2[0])
        js_display_print(0, 9, banner2, 1);

    int shown = nrows - *scroll;
    if (shown > BS_VISIBLE_ROWS)
        shown = BS_VISIBLE_ROWS;

    for (int i = 0; i < shown; i++) {
        int row_idx = *scroll + i;
        char line[72];
        snprintf(line, sizeof(line), "%s%s",
                 (row_idx == cursor) ? "> " : "  ", rows[row_idx].name);
        js_display_print(0, 20 + i * 9, line, 1);
    }

    /* House rule: every scrolling list draws a scrollbar (and no arrows). */
    if (nrows > BS_VISIBLE_ROWS) {
        int track_y = 20, track_h = BS_VISIBLE_ROWS * 9 - 2;
        int thumb_h = track_h * BS_VISIBLE_ROWS / nrows;
        if (thumb_h < 4)
            thumb_h = 4;
        int thumb_y = track_y + (track_h - thumb_h) * *scroll / (nrows - BS_VISIBLE_ROWS);
        js_display_draw_line(126, track_y, 126, track_y + track_h, 1);
        js_display_fill_rect(125, thumb_y, 3, thumb_h, 1);
    }

    js_display_print(0, 56, "Click: boot   Back: cancel", 1);
}

/* Runs the boot window's ~2s clock. Returns 1 if Back was pressed (edge),
 * 0 on timeout. */
static int bs_run_window(int fd, uint8_t *map, bs_input_state_t *st) {
    for (int i = 0; i < BS_WINDOW_FRAMES; i++) {
        bs_pump_frame(fd);
        bs_serve_display(map);
        bs_input_events_t ev;
        bs_scan_input(map, st, &ev);
        if (ev.back_pressed)
            return 1;
    }
    return 0;
}

/* Single exit path for every success outcome: print `id`, blank the screen
 * (so Move doesn't inherit stale pixels), unmap, close, flush stdout, exit 0. */
static void bs_finish(int fd, uint8_t *map, const char *id) {
    /* Disarm the SIGALRM wedge backstop FIRST: it fires by writing the
     * incoming id straight to stdout, and a signal landing after our own
     * printf below (but before we've disarmed) would put a second line on
     * stdout — the caller reads exactly one and boots garbage. */
    alarm(0);

    printf("%s\n", id);
    fflush(stdout);

    /* Blank the screen through the same pull handshake (a push is ignored):
     * serve zeros for ~2 full request cycles so Move does not inherit stale
     * pixels, then stop responding. */
    js_display_clear();
    bs_repaint();
    for (int i = 0; i < 16; i++) {
        bs_pump_frame(fd);
        bs_serve_display(map);
    }
    memset(map + SCHWUNG_OFF_OUT_DISP_STAT, 0, 4);

    munmap(map, SCHWUNG_PAGE_SIZE);
    close(fd);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s --window|--forced <id>\n", argv[0]);
        return 1;
    }

    int forced;
    if (strcmp(argv[1], "--forced") == 0) {
        forced = 1;
    } else if (strcmp(argv[1], "--window") == 0) {
        forced = 0;
    } else {
        fprintf(stderr, "boot-select: unknown mode '%s'\n", argv[1]);
        return 1;
    }

    const char *incoming_id = argv[2];
    snprintf(g_incoming_id, sizeof(g_incoming_id), "%s", incoming_id);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bs_alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(BS_ALARM_SECONDS);

    const char *targets_dir = getenv("BOOT_TARGETS_DIR");
    if (!targets_dir || !targets_dir[0])
        targets_dir = "/data/UserData/boot-targets";

    bs_row_t rows[BS_MAX_ROWS];
    int nrows = bs_build_rows(targets_dir, rows, BS_MAX_ROWS);
    if (nrows <= 0) {
        fprintf(stderr, "boot-select: no boot targets found in %s\n", targets_dir);
        return 1;
    }

    int fd = open(SCHWUNG_SPI_DEVICE, O_RDWR);
    if (fd == -1)
        bs_die("open");

    unsigned char *map = mmap(NULL, SCHWUNG_PAGE_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        bs_die("mmap");
    }

    /* Zero the whole TX/RX page once. We only ever write display bytes
     * (SCHWUNG_OFF_OUT_DISP_STAT / _DATA) after this — the MIDI OUT region
     * we never touch stays zero for the whole run. */
    memset(map, 0, SCHWUNG_PAGE_SIZE);

    ioctl(fd, _IOC(_IOC_NONE, 0, SCHWUNG_IOCTL_SET_SPEED, 0), 0x1312d00);

    bs_cancel_led_anims(fd, map);

    bs_input_state_t input_state;
    memset(&input_state, 0, sizeof(input_state));

    int found_idx = bs_find_row(rows, nrows, incoming_id);
    const char *incoming_name = (found_idx >= 0) ? rows[found_idx].name : incoming_id;
    int cursor = forced ? (nrows - 1) : (found_idx >= 0 ? found_idx : 0);

    if (!forced) {
        bs_draw_window(incoming_name);
        bs_repaint();

        int back_pressed = bs_run_window(fd, map, &input_state);
        if (!back_pressed)
            bs_finish(fd, map, incoming_id); /* timeout: boot the incoming id */
        /* fall through into the picker */
    }

    char banner1[96];
    const char *banner2;
    if (forced) {
        snprintf(banner1, sizeof(banner1), "%s", incoming_name);
        banner2 = "failed to start";
    } else {
        snprintf(banner1, sizeof(banner1), "Select boot target");
        banner2 = "";
    }

    int scroll = 0;
    bs_draw_picker(banner1, banner2, rows, nrows, cursor, &scroll);
    bs_repaint();

    int jog_accum = 0;
    for (;;) {
        bs_pump_frame(fd);
        bs_serve_display(map);
        bs_input_events_t ev;
        bs_scan_input(map, &input_state, &ev);

        if (ev.back_pressed)
            bs_finish(fd, map, incoming_id); /* cancel: leave the id unchanged */

        int dirty = 0;
        if (ev.jog_delta != 0) {
            jog_accum += ev.jog_delta;
            while (jog_accum >= 1 && cursor < nrows - 1) {
                cursor++;
                jog_accum--;
                dirty = 1;
            }
            while (jog_accum <= -1 && cursor > 0) {
                cursor--;
                jog_accum++;
                dirty = 1;
            }
            /* Pinned at an edge: drop the rest of a long spin rather than
             * letting it buffer and fire once the cursor can move again. */
            if (cursor == nrows - 1 && jog_accum > 0)
                jog_accum = 0;
            if (cursor == 0 && jog_accum < 0)
                jog_accum = 0;
        }

        if (ev.click_pressed) {
            const char *chosen_id = rows[cursor].id;
            if (!bs_write_default(targets_dir, chosen_id)) {
                fprintf(stderr, "boot-select: failed to write %s/default: %s\n",
                        targets_dir, strerror(errno));
                /* Still report the chosen id so the caller boots what the
                 * user asked for even if persistence failed. */
            }
            bs_finish(fd, map, chosen_id);
        }

        if (dirty) {
            bs_draw_picker(banner1, banner2, rows, nrows, cursor, &scroll);
            bs_repaint();
        }
    }

    return 0; /* unreachable */
}
