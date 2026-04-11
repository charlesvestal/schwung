# Move SPI Protocol Reference

Reverse-engineered from Ableton's JACK Move driver (`jack2.move-move/linux/move/move-spi/`) and RNBO's `jack_move.so`.

## SPI Transfer

- Device: `/dev/ablspi0.0`
- mmap: 4096 bytes
- Transfer size: **768 bytes** per ioctl
- SPI clock: **20 MHz**
- ioctl command: `ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE` (command 10)

## Buffer Layout

```
OUTPUT (TX) — mmap offset 0:
  Offset  Size    Content
  0       80      MIDI OUT: 20 × 4-byte USB-MIDI messages
  80      4       Display status word
  84      172     Display data chunk
  256     512     Audio OUT: 128 frames × 2 channels × int16

INPUT (RX) — mmap offset 2048:
  Offset  Size    Content
  2048    248     MIDI IN: 31 × 8-byte AblSpiMidiEvent
  2296    4       Display status word
  2304    512     Audio IN: 128 frames × 2 channels × int16
```

Audio: 44100 Hz, 128 frames/block, stereo interleaved int16, little-endian.

## MIDI Event Formats

### MIDI OUT (TX): 4-byte USB-MIDI packets

Standard USB-MIDI format. Max 20 messages per transfer. Unused slots zeroed.

```c
typedef struct {
    uint8_t cin : 4;      // Code Index Number (0x09=note-on, 0x08=note-off, 0x0B=CC)
    uint8_t cable : 4;    // Cable number
    uint8_t status;       // MIDI status byte
    uint8_t data1;        // Note/CC number
    uint8_t data2;        // Velocity/CC value
} AblSpiUsbMidiMessage;   // 4 bytes
```

### MIDI IN (RX): 8-byte events

**This is critical.** Each MIDI IN event is 8 bytes, NOT 4.

```c
typedef struct __attribute__((packed)) {
    AblSpiUsbMidiMessage message;  // 4 bytes (USB-MIDI)
    uint32_t timestamp;            // 4 bytes
} AblSpiMidiEvent;                 // 8 bytes total
```

Injecting 4-byte packets into MIDI_IN with 4-byte stride causes misalignment — the second event lands in the timestamp field of the first slot. This causes SIGABRT from Move's ProcessEventsStepper.

**Correct injection:** Write 8-byte events with 8-byte stride. Set timestamp bytes to monotonically increasing values (scan existing events for max timestamp, inject at max+1).

Empty event detection: `(word & 0xFF) == 0` (low byte zero).

## Cable Numbers

| Cable | Direction | Purpose |
|-------|-----------|---------|
| 0 | IN/OUT | Internal Move hardware controls (pads, knobs, buttons) |
| 2 | IN/OUT | External USB MIDI (devices on Move's USB-A port) |
| 14 | OUT | System-level events |
| 15 | OUT | SPI protocol-bound events |

## Display Protocol

- Total framebuffer: 1024 bytes
- Sent in **6 chunks** of 172 bytes each
- Double-buffered in the device struct
- Index handshake: hardware sends display status index via RX, driver echoes it back and sends the corresponding chunk
  - Index 1-5: send chunk `(index-1) * 172` bytes
  - Index 6: send final chunk (remaining bytes)

## ioctl Commands

```c
enum IoctlCommands {
    ABLSPI_FILL_TX_BUFFER = 0,
    ABLSPI_FILL_RX_BUFFER = 1,
    ABLSPI_READ_BUFFER = 2,
    ABLSPI_SEND_MESSAGE = 3,
    ABLSPI_SEND_MESSAGE_AND_WAIT = 4,
    ABLSPI_WAIT_AND_SEND_MESSAGE = 5,
    ABLSPI_GET_STATE = 6,
    ABLSPI_CAN_SEND = 7,
    ABLSPI_SET_MESSAGE_SIZE = 8,
    ABLSPI_GET_MESSAGE_SIZE = 9,
    ABLSPI_WAIT_AND_SEND_MESSAGE_WITH_SIZE = 10,
    ABLSPI_SET_SPEED = 11,
    ABLSPI_GET_SPEED = 12
};
```

## Key Constants

```c
#define ABLSPI_AUDIO_BUFFER_SIZE      128
#define ABLSPI_AUDIO_SAMPLE_RATE_HZ   44100
#define ABLSPI_MAX_MIDI_IN_PER_TRANSFER  31
#define ABLSPI_MAX_MIDI_OUT_PER_TRANSFER 20
#define ABLSPI_DISPLAY_BYTES          1024
```

## XMOS Heartbeat

Position 248 in MIDI_IN always contains an XMOS heartbeat event (CIN 1-3, status 0x00). Must be cleared/skipped when injecting cable-2 events.

## Rate Limiting

Injecting too many MIDI events per frame causes SIGABRT. Safe limits:
- MIDI_IN injection: 4-8 events per frame
- RTP-MIDI injection: 8 events per tick (>16 causes SIGABRT)

## Implementation Notes

- MIDI OUT: `handleMidiOutput()` writes up to 20 messages, zeros remaining slots with memset
- MIDI IN: `handleMidiInput()` iterates 8-byte events, stops at first empty message
- SPI transfer blocks until hardware is ready (the ioctl itself takes ~2ms)
- Frame budget after ioctl: ~900µs for all shim/host processing
