// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Charles Vestal
//
// Schwung SPI Library — LD_PRELOAD interception for Ableton Move.
//
// SPI buffer layout and protocol derived from Ableton's ablspi library
// (GPL-2.0, obtained via GPL source request for Move's JACK driver).
// Defines buffer offsets, MIDI types, and provides LD_PRELOAD hooks
// with shadow buffer management.
//
// Usage:
//   1. Compile this library into your LD_PRELOAD .so
//   2. Register pre/post transfer callbacks for your domain logic
//   3. Access the shadow buffer via schwung_spi_get_shadow()
//   The library handles: open/mmap/ioctl hooks, shadow↔hardware buffer copies,
//   EINTR retry on blocking ioctl.
//
// For move-everything compatibility:
//   #define global_mmap_addr    schwung_spi_get_shadow(g_spi)
//   #define hardware_mmap_addr  schwung_spi_get_hw(g_spi)
//   Then all existing domain code works unchanged.

#ifndef SCHWUNG_SPI_LIB_H
#define SCHWUNG_SPI_LIB_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// SPI Protocol Constants (from Ableton ablspi reference)
// ============================================================================

#define SCHWUNG_SPI_DEVICE         "/dev/ablspi0.0"
#define SCHWUNG_PAGE_SIZE          4096
#define SCHWUNG_FRAME_SIZE         768
#define SCHWUNG_SPI_FREQ           20000000
#define SCHWUNG_AUDIO_FRAMES       128
#define SCHWUNG_SAMPLE_RATE        44100

// Buffer layout — output (host → XMOS)
#define SCHWUNG_OFF_OUT_MIDI       0
#define SCHWUNG_OFF_OUT_DISP_STAT  80
#define SCHWUNG_OFF_OUT_DISP_DATA  84
#define SCHWUNG_OFF_OUT_AUDIO      256
#define SCHWUNG_OUT_DISP_CHUNK_LEN 172

// Buffer layout — input (XMOS → host)
#define SCHWUNG_OFF_IN_BASE        2048
#define SCHWUNG_OFF_IN_MIDI        (SCHWUNG_OFF_IN_BASE)
#define SCHWUNG_OFF_IN_DISP_STAT   (SCHWUNG_OFF_IN_BASE + 248)
#define SCHWUNG_OFF_IN_AUDIO       (SCHWUNG_OFF_IN_BASE + 256)

// MIDI limits
#define SCHWUNG_MIDI_IN_MAX        31
#define SCHWUNG_MIDI_OUT_MAX       20

// Display
#define SCHWUNG_DISPLAY_SIZE       1024

// ioctl command numbers
enum schwung_ioctl_cmd {
    SCHWUNG_IOCTL_FILL_TX        = 0,
    SCHWUNG_IOCTL_FILL_RX        = 1,
    SCHWUNG_IOCTL_READ_BUF       = 2,
    SCHWUNG_IOCTL_SEND           = 3,
    SCHWUNG_IOCTL_SEND_WAIT      = 4,
    SCHWUNG_IOCTL_WAIT_SEND      = 5,
    SCHWUNG_IOCTL_GET_STATE      = 6,
    SCHWUNG_IOCTL_CAN_SEND       = 7,
    SCHWUNG_IOCTL_SET_MSG_SIZE   = 8,
    SCHWUNG_IOCTL_GET_MSG_SIZE   = 9,
    SCHWUNG_IOCTL_WAIT_SEND_SIZE = 10,
    SCHWUNG_IOCTL_SET_SPEED      = 11,
    SCHWUNG_IOCTL_GET_SPEED      = 12,
};

// ============================================================================
// MIDI Types
// ============================================================================

typedef struct {
    uint8_t channel : 4;
    uint8_t type    : 4;
    uint8_t data1;
    uint8_t data2;
} SchwungMidiMsg;

typedef struct {
    uint8_t cin   : 4;
    uint8_t cable : 4;
    SchwungMidiMsg midi;
} SchwungUsbMidiMsg;

typedef struct __attribute__((packed)) {
    SchwungUsbMidiMsg message;    // 4 bytes
    uint32_t          timestamp;  // 4 bytes
} SchwungMidiEvent;

// ============================================================================
// Library API
// ============================================================================

typedef struct SchwungSpi SchwungSpi;

// Pre-transfer callback: called before shadow→hardware copy.
// Domain logic should write audio, MIDI, display into the shadow buffer.
typedef void (*schwung_spi_pre_fn)(void *ctx, uint8_t *shadow, int size);

// Post-transfer callback: called after hardware→shadow copy.
// Domain logic can read fresh MIDI input, filter MIDI_IN, etc.
// Both shadow (updated from hw) and raw hw buffer are available.
typedef void (*schwung_spi_post_fn)(void *ctx, uint8_t *shadow, const uint8_t *hw, int size);

// Initialize the library. Call from __attribute__((constructor)).
SchwungSpi *schwung_spi_init(void);

// Register pre/post transfer callbacks.
// Must be called before SPI device is opened (typically in constructor).
void schwung_spi_set_callbacks(SchwungSpi *spi,
                                schwung_spi_pre_fn pre,
                                schwung_spi_post_fn post,
                                void *ctx);

// Get the shadow buffer (what Move reads/writes). NULL before mmap.
uint8_t *schwung_spi_get_shadow(SchwungSpi *spi);

// Get the hardware buffer (raw mmap'd SPI). NULL before mmap.
uint8_t *schwung_spi_get_hw(SchwungSpi *spi);

// Get the SPI file descriptor. -1 before open.
int schwung_spi_get_fd(SchwungSpi *spi);

// Check if SPI device has been mapped.
int schwung_spi_is_ready(SchwungSpi *spi);

// Log a message to /data/UserData/schwung/schwung.log
void schwung_spi_log(const char *msg);

// ============================================================================
// MIDI Helpers
// ============================================================================

int schwung_midi_msg_is_empty(SchwungMidiMsg msg);
int schwung_usb_midi_msg_is_empty(SchwungUsbMidiMsg msg);
SchwungUsbMidiMsg schwung_encode_usb_midi(SchwungMidiMsg msg, uint8_t cable);

// ============================================================================
// Audio Helpers
// ============================================================================

void schwung_deinterleave_stereo(const int16_t *interleaved,
                                  int16_t *left, int16_t *right, int frames);
void schwung_interleave_stereo(const int16_t *left, const int16_t *right,
                                int16_t *interleaved, int frames);
void schwung_mix_interleaved(int16_t *dest, const int16_t *src, int frames);

#endif // SCHWUNG_SPI_LIB_H
