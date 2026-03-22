// schwung_jack_shm.h — Shared memory layout for JACK shadow driver
// Used by both the schwung shim and jack_shadow.so JACK driver.
// No dependencies beyond stdint.h and stddef.h.
#ifndef SCHWUNG_JACK_SHM_H
#define SCHWUNG_JACK_SHM_H

#include <stdint.h>
#include <stddef.h>

#define SCHWUNG_JACK_SHM_PATH    "/schwung_jack"
#define SCHWUNG_JACK_SHM_SIZE    4096
#define SCHWUNG_JACK_MAGIC       0x534A434B  /* "SJCK" */
#define SCHWUNG_JACK_VERSION     1

#define SCHWUNG_JACK_AUDIO_FRAMES   128
#define SCHWUNG_JACK_MIDI_IN_MAX    31
#define SCHWUNG_JACK_MIDI_OUT_MAX   64
#define SCHWUNG_JACK_DISPLAY_SIZE   1024

/* Raw SPI MIDI types — must match schwung_spi_lib.h */
typedef struct {
    uint8_t channel : 4;
    uint8_t type    : 4;
    uint8_t data1;
    uint8_t data2;
} SchwungJackMidiMsg;

typedef struct {
    uint8_t cin   : 4;
    uint8_t cable : 4;
    SchwungJackMidiMsg midi;
} SchwungJackUsbMidiMsg;

typedef struct __attribute__((packed)) {
    SchwungJackUsbMidiMsg message;
    uint32_t              timestamp;
} SchwungJackMidiEvent;

/* Shared memory layout — single 4096-byte page.
 * No packed on outer struct — all fields naturally aligned at page-aligned mmap. */
typedef struct {
    /* Header (16 bytes) */
    uint32_t magic;
    uint32_t version;
    uint32_t frame_counter;
    uint32_t jack_frame_done;

    /* Audio — stereo interleaved int16 (1024 bytes) */
    int16_t audio_out[SCHWUNG_JACK_AUDIO_FRAMES * 2];
    int16_t audio_in[SCHWUNG_JACK_AUDIO_FRAMES * 2];

    /* MIDI cable 0 — Move hardware (330 bytes) */
    SchwungJackMidiEvent    midi_to_jack[SCHWUNG_JACK_MIDI_IN_MAX];
    uint8_t                 midi_to_jack_count;
    SchwungJackUsbMidiMsg   midi_from_jack[SCHWUNG_JACK_MIDI_OUT_MAX];
    uint8_t                 midi_from_jack_count;

    /* MIDI cable 2+ — external USB (330 bytes) */
    SchwungJackMidiEvent    ext_midi_to_jack[SCHWUNG_JACK_MIDI_IN_MAX];
    uint8_t                 ext_midi_to_jack_count;
    SchwungJackUsbMidiMsg   ext_midi_from_jack[SCHWUNG_JACK_MIDI_OUT_MAX];
    uint8_t                 ext_midi_from_jack_count;

    /* Display (1025 bytes) */
    uint8_t display_data[SCHWUNG_JACK_DISPLAY_SIZE];
    uint8_t display_active;

} SchwungJackShm;

#ifdef __cplusplus
static_assert(sizeof(SchwungJackShm) <= SCHWUNG_JACK_SHM_SIZE,
              "SchwungJackShm exceeds 4096 bytes");
#else
_Static_assert(sizeof(SchwungJackShm) <= SCHWUNG_JACK_SHM_SIZE,
               "SchwungJackShm exceeds 4096 bytes");
#endif

#endif /* SCHWUNG_JACK_SHM_H */
