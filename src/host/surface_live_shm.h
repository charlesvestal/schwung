/*
 * surface_live_shm.h -- what Move's control surface is doing, for the web mirror.
 *
 * Written by the shim on the SPI callback, read by display_server, which
 * streams it beside the OLED so /mirror can draw the whole device: every LED
 * as the hardware was last told to light it, and every control as the hand is
 * holding it.
 *
 * Both halves are read at the ONE point where they are the truth:
 *
 *   LEDs     the final MIDI_OUT, at the end of shim_pre_transfer (PREEND) --
 *            Move's writes, Schwung's LED queue and an overtake module's all
 *            merged, which is what the XMOS receives. Reading Move's writes
 *            alone would draw Move's intent under a Schwung screen.
 *   presses  the raw hardware MIDI_IN, at the top of shim_post_transfer --
 *            before any blocking site swallows an event or an injection adds
 *            one, so a press Schwung withholds from Move still shows.
 *
 * Only cable 0 is read (Move's own surface). An LED value is a palette index
 * for an RGB LED and a brightness for a white one; the status CHANNEL is the
 * animation (0 solid, 1-5 transition, 6-10 pulse, 11-15 blink). Move also
 * paints LEDs with an RGB SysEx, F0 00 21 1D 01 01 3B <ch<<4> <idx> r g b F7
 * (each colour a 7-bit lo/hi pair), which overrides the palette. The <ch> byte
 * says which namespace <idx> is in: 0 = a NOTE (pads 68-99, steps 16-31),
 * 1 = a CC (tracks, knob rings, transport). It reads like a subcommand, and
 * decoding only 0x10 would miss every pad -- Cycling '74's MIT
 * move_midi_emulator.html (rnbo.move.templates) is the reference for this.
 *
 * Realtime: plain stores into a mapped page, no syscalls, no allocation, and
 * the sequence counter is bumped only in a frame where something changed.
 * Torn-read protection is the same as e16_mirror_shm.h: the writer makes `seq`
 * ODD before writing and EVEN after; a reader copies only between two equal,
 * even reads.
 */
#ifndef SURFACE_LIVE_SHM_H
#define SURFACE_LIVE_SHM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SURFACE_LIVE_SHM_NAME  "/schwung-surface-live"
#ifndef SURFACE_LIVE_SHM_PATH   /* tests point it at a plain file */
#define SURFACE_LIVE_SHM_PATH  "/dev/shm/schwung-surface-live"
#endif
#define SURFACE_LIVE_MAGIC     "SURFLV1"
#define SURFACE_LIVE_VERSION   1
#define SURFACE_LIVE_EVENTS    32
#define SURFACE_LIVE_ANIM_NONE 0xFF   /* an LED no one has written since boot */

/* Relative encoders keep a running detent count, so a viewer can show which
 * way a knob is going: index 0-7 = knobs 1-8 (CC 71-78), 8 = volume (CC 79),
 * 9 = jog (CC 14). */
#define SURFACE_LIVE_ENCODERS  10

typedef struct {
    uint32_t frame;                /* surface_live_shm_t.frame when it arrived */
    uint8_t status, d1, d2, pad;
} surface_live_event_t;

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t seq;                  /* odd while writing */
    uint32_t frame;                /* SPI frames since init (~2.9 ms each); outside the seq */
    uint32_t event_count;          /* input events ever recorded; ring head */
    uint8_t note_led[128];         /* value last sent; 0 = off */
    uint8_t note_led_anim[128];    /* status channel, or SURFACE_LIVE_ANIM_NONE */
    uint8_t cc_led[128];
    uint8_t cc_led_anim[128];
    uint8_t rgb[2][128][4];        /* SysEx RGB by [ch][idx]: r, g, b, valid.
                                    * `valid` means the RGB write is the LATEST
                                    * for that LED: a later palette write to the
                                    * same note/CC clears it, as it would repaint
                                    * the hardware. */
    uint8_t note_down[128];        /* velocity while held, 0 when up */
    uint8_t note_pressure[128];    /* last poly aftertouch while held */
    uint8_t cc_value[128];         /* last value received */
    int16_t enc_pos[SURFACE_LIVE_ENCODERS];
    uint8_t reserved[12];
    surface_live_event_t events[SURFACE_LIVE_EVENTS];  /* events[n % N] */
} surface_live_shm_t;

/* Two processes map this, so the layout is pinned rather than trusted. The
 * browser decodes the same bytes (display_server ships them base64), so a
 * change here is a change to the page too -- bump SURFACE_LIVE_VERSION. */
_Static_assert(offsetof(surface_live_shm_t, seq) == 12, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, frame) == 16, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, note_led) == 24, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, rgb) == 24 + 4 * 128, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, note_down) == 24 + 12 * 128, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, enc_pos) == 24 + 15 * 128, "surface live layout");
_Static_assert(offsetof(surface_live_shm_t, events) == 24 + 15 * 128 + 32, "surface live layout");
_Static_assert(sizeof(surface_live_shm_t) == 24 + 15 * 128 + 32 + SURFACE_LIVE_EVENTS * 8,
               "surface live layout");

/* Writer-local state: the SysEx reassembly spans packets and frames, and is
 * nobody else's business, so it is not in the shared page. */
typedef struct {
    uint8_t buf[24];
    int len;
    int active;
    int writing;                   /* seq is odd: a begin with no end yet */
} surface_live_writer_t;

static inline void surface_live_init(surface_live_shm_t *s, surface_live_writer_t *w) {
    memset(s, 0, sizeof(*s));
    memcpy(s->magic, SURFACE_LIVE_MAGIC, sizeof(SURFACE_LIVE_MAGIC));
    s->version = SURFACE_LIVE_VERSION;
    memset(s->note_led_anim, SURFACE_LIVE_ANIM_NONE, sizeof(s->note_led_anim));
    memset(s->cc_led_anim, SURFACE_LIVE_ANIM_NONE, sizeof(s->cc_led_anim));
    memset(w, 0, sizeof(*w));
}

/* Opened lazily, on the first change of a frame, so an idle frame costs no
 * store to the shared page at all. */
static inline void surface_live_begin(surface_live_shm_t *s, surface_live_writer_t *w) {
    if (w->writing) return;
    w->writing = 1;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void surface_live_end(surface_live_shm_t *s, surface_live_writer_t *w) {
    if (!w->writing) return;
    w->writing = 0;
    __atomic_store_n(&s->seq, s->seq + 1, __ATOMIC_RELEASE);
}

static inline void surface_live_sysex_byte_run(surface_live_shm_t *s, surface_live_writer_t *w,
                                               const uint8_t *b, int n, int ends) {
    for (int i = 0; i < n; i++) {
        if (b[i] == 0xF0) { w->len = 0; w->active = 1; }
        if (!w->active) continue;
        if (w->len < (int)sizeof(w->buf)) w->buf[w->len++] = b[i];
        else w->active = 0;                     /* too long to be an LED: drop */
    }
    if (!ends || !w->active) return;
    static const uint8_t hdr[7] = { 0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x3B };
    /* F0 00 21 1D 01 01 3B ch idx rl rh gl gh bl bh F7 = 16 bytes */
    if (w->len == 16 && memcmp(w->buf, hdr, sizeof hdr) == 0 && w->buf[15] == 0xF7 &&
        (w->buf[7] == 0x00 || w->buf[7] == 0x10) && w->buf[8] < 128) {
        uint8_t *px = s->rgb[w->buf[7] >> 4][w->buf[8]];
        uint8_t r = (uint8_t)(w->buf[9]  | (w->buf[10] << 7));
        uint8_t g = (uint8_t)(w->buf[11] | (w->buf[12] << 7));
        uint8_t b2 = (uint8_t)(w->buf[13] | (w->buf[14] << 7));
        if (px[0] != r || px[1] != g || px[2] != b2 || !px[3]) {
            surface_live_begin(s, w);
            px[0] = r; px[1] = g; px[2] = b2; px[3] = 1;
        }
    }
    w->active = 0;
    w->len = 0;
}

/* The final MIDI_OUT (4-byte USB-MIDI packets). */
static inline void surface_live_scan_out(surface_live_shm_t *s, surface_live_writer_t *w,
                                         const uint8_t *midi_out, int bytes) {
    for (int i = 0; i + 3 < bytes; i += 4) {
        const uint8_t *p = midi_out + i;
        if ((p[0] >> 4) != 0) continue;          /* cable 0 only */
        uint8_t cin = p[0] & 0x0F;
        uint8_t type = p[1] & 0xF0;
        if (cin >= 0x04 && cin <= 0x07) {
            int n = (cin == 0x04) ? 3 : cin - 0x04;
            surface_live_sysex_byte_run(s, w, p + 1, n, cin != 0x04);
            continue;
        }
        if (p[2] > 127) continue;
        uint8_t anim = p[1] & 0x0F;
        if ((cin == 0x09 && type == 0x90) || (cin == 0x08 && type == 0x80)) {
            uint8_t v = (type == 0x80) ? 0 : p[3];
            if (type == 0x80) anim = 0;
            if (s->note_led[p[2]] != v || s->note_led_anim[p[2]] != anim || s->rgb[0][p[2]][3]) {
                surface_live_begin(s, w);
                s->note_led[p[2]] = v;
                s->note_led_anim[p[2]] = anim;
                s->rgb[0][p[2]][3] = 0;
            }
        } else if (cin == 0x0B && type == 0xB0) {
            if (s->cc_led[p[2]] != p[3] || s->cc_led_anim[p[2]] != anim || s->rgb[1][p[2]][3]) {
                surface_live_begin(s, w);
                s->cc_led[p[2]] = p[3];
                s->cc_led_anim[p[2]] = anim;
                s->rgb[1][p[2]][3] = 0;
            }
        }
    }
    surface_live_end(s, w);
}

static inline int surface_live_encoder_index(uint8_t cc) {
    if (cc >= 71 && cc <= 79) return cc - 71;
    if (cc == 14) return 9;
    return -1;
}

static inline void surface_live_record_event(surface_live_shm_t *s, const uint8_t *p) {
    surface_live_event_t *e = &s->events[s->event_count % SURFACE_LIVE_EVENTS];
    e->frame = s->frame;
    e->status = p[1]; e->d1 = p[2]; e->d2 = p[3]; e->pad = 0;
    s->event_count++;
}

/* The raw hardware MIDI_IN (8-byte stride: USB-MIDI packet + timestamp).
 * Advances the frame counter, so call it exactly once per SPI frame. */
static inline void surface_live_scan_in(surface_live_shm_t *s, surface_live_writer_t *w,
                                        const uint8_t *midi_in, int bytes) {
    __atomic_store_n(&s->frame, s->frame + 1, __ATOMIC_RELAXED);
    for (int i = 0; i + 7 < bytes; i += 8) {
        const uint8_t *p = midi_in + i;
        if ((p[0] >> 4) != 0) continue;          /* cable 0 only */
        uint8_t cin = p[0] & 0x0F;
        uint8_t type = p[1] & 0xF0;
        uint8_t d1 = p[2], d2 = p[3];
        if (d1 > 127 || d2 > 127) continue;
        if (cin == 0x09 && type == 0x90) {
            surface_live_begin(s, w);
            s->note_down[d1] = d2;
            if (!d2) s->note_pressure[d1] = 0;
            surface_live_record_event(s, p);
        } else if (cin == 0x08 && type == 0x80) {
            surface_live_begin(s, w);
            s->note_down[d1] = 0;
            s->note_pressure[d1] = 0;
            surface_live_record_event(s, p);
        } else if (cin == 0x0A && type == 0xA0) {
            if (s->note_pressure[d1] != d2) {
                surface_live_begin(s, w);
                s->note_pressure[d1] = d2;
            }
        } else if (cin == 0x0B && type == 0xB0) {
            surface_live_begin(s, w);
            s->cc_value[d1] = d2;
            int enc = surface_live_encoder_index(d1);
            if (enc >= 0) {
                /* 1-63 clockwise, 65-127 counter-clockwise (two's complement) */
                s->enc_pos[enc] = (int16_t)(s->enc_pos[enc] + (d2 < 64 ? d2 : d2 - 128));
            } else {
                /* A turn is state, not an event: spinning a knob would flush
                 * every press out of a 32-slot ring in a second. */
                surface_live_record_event(s, p);
            }
        }
    }
    surface_live_end(s, w);
}

#endif /* SURFACE_LIVE_SHM_H */
