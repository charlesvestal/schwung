# Move SPI Protocol Reference

Reverse-engineered from Ableton's JACK Move driver (`jack2.move-move/linux/move/move-spi/`) and RNBO's `jack_move.so`.

The **driver side** is no longer reverse-engineered: `ablspi` ships in Ableton's
GPL source release for Move, as `drivers/spi/ablspi-{core,gpio,proc}.c` inside
the patched `linux-raspberrypi-5.15.92-rt57` tree (`CONFIG_ABLSPI=m`, device
tree `arch/arm64/boot/dts/overlays/ablspi-overlay.dts`, IRQ on GPIO 3). Anything
below marked *(driver)* is read from that source rather than inferred.

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

KERNEL (driver-written) — end of the page:
  4088    8       struct ablspi_sys_info { u64 spi_tx_time; }
```

Audio: 44100 Hz, 128 frames/block, stereo interleaved int16, little-endian.

**RX is not a clean 2048 bytes** *(driver)*. `ablspi_open` splits the page in
half — `tx_buffer` at 0, `rx_buffer` at `PAGE_SIZE/2` — and then places
`sysinfo` at `PAGE_SIZE - sizeof(struct ablspi_sys_info)`, i.e. **inside the
tail of the RX half**. RX is therefore safely 2040 bytes, not 2048. Harmless at
Move's 768-byte frames, which reach nowhere near it, but a larger transfer would
have the driver overwrite its own telemetry.

### spi_tx_time — free per-frame transfer timing *(driver)*

The driver stamps the real duration of each transfer, in nanoseconds:

```c
s64 then = trace_clock_local();
ablspi_send_message(spidev, spi, arg);
wait_for_completion(&spidev->dma_transfer_finished);
spidev->sysinfo->spi_tx_time = trace_clock_local() - then;
```

It is written before the ioctl returns, so by the time `shim_post_transfer` runs
the value describes the frame that just completed. Reading it costs one aligned
8-byte load of memory the shim already maps — no syscall, nothing on the wire.
`SCHWUNG_OFF_SPI_TX_TIME` in `src/lib/schwung_spi_lib.h`; consumed by
`src/host/spi_tally.c`. The field reads 0 until the first transfer completes.

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

**Only 6, 7, 10, 11 and 12 exist in the driver** *(driver)* — the others above
are ours and the kernel implements nothing for them. Dispatch is on
`_IOC_NR(cmd)`, so the direction/size/type bits are ignored and Move passes the
bare integers (which is what lets the shim's `ioctl` hook compare `request`
directly). Constraints the driver enforces:

- `WAIT_AND_SEND_MESSAGE_WITH_SIZE`: `arg` is the transfer size, and must be
  128 … `PAGE_SIZE/2`. Rejected with `-EINVAL` if a transfer is already in
  flight, or if the IRQ GPIO never probed.
- `SET_SPEED`: 5 MHz … 25 MHz. 5 MHz is the driver default; Move selects 20, so
  there is headroom.
- Only **one** process may hold the device — a second `open` gets `-EBUSY`.
  This is why the shim has to be an `LD_PRELOAD` inside MoveOriginal rather
  than a second reader.
- `read()` and `write()` are stubs that return 0. All I/O is mmap + ioctl.

### The IRQ is a counting semaphore, not a flag *(driver)*

`ablspi_gpio_isr` does `atomic_inc(&spidev->irq_arrived)`;
`ablspi_wait_for_interrupt` does `atomic_dec`. So **an overrun does not drop a
frame — it queues.** If we miss our budget, the IRQs that fired meanwhile are
still counted, and the next waits return immediately, replaying back-to-back.

The consequence for diagnosis is large: a late frame is invisible as a gap and
shows up as a **burst**, which is easy to attribute to some other producer
misbehaving. `/proc/ableton/ablspi0.0/irq_count` (bumped by the ISR regardless
of whether we serviced it) minus our own frame count is exactly that queue —
see "SPI frame tally" in CLAUDE.md.

`/proc/ableton/ablspi0.0/` also carries `spi_tx_time` (same value as the mmap
field) and `failed_send_count`, which **nothing in the driver ever increments**
— it is always 0 and means nothing.

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

## USB-C Audio-Out Source (XMOS audio-IO SysEx)

Moved here from `CLAUDE.md`, which keeps a summary. This is the `37 12` /
`37 14` TLV pair on MIDI_OUT cable 0, and the boot arbitration around it.

Move's Settings menu picks what a connected computer receives over USB-C (Mic or
Main Out). Move's firmware **never persists it** — there is no key in
`/data/UserData/settings/Settings.json`, and the dialog is built as
`ListViewDelegate<UsbAudioOutputSourceDelegate, NullTransactionPolicy>` — so it
reverts to Mic on every boot. Schwung remembers it instead.

Selecting a value makes Move emit a **pair** of XMOS audio-IO SysEx messages in
one SPI frame (captured on hardware 2026-08-18):

```
Main Out:  F0 00 21 1D 01 01 37 12 02 00×12 F7   +   F0 00 21 1D 01 01 37 14 01 00×12 F7
Mic:       ...37 12 00...                        +   ...37 14 00...
```

`37 12` is the shared routing/monitoring TLV — **bit0 is the USB-C *input*
select, owned by Move's sampling page**; bit1 is monitoring, which is *how* Main
Out reaches USB-C (the XMOS mutes the speakers while it's set, to prevent
feedback). `37 14` is the dedicated out-source bit. This resolves open question
Q2 in the movesniff findings doc, which listed `0x14` as unreversed.

**Move's sampling page emits a LONE `37 12`, and it clears bit1.** Captured
2026-08-26: changing the sampling source sent `37 12 01` then `37 12 00`, with
no `37 14` anywhere near either. The original 2026-08-18 capture recorded bit0
as `0` throughout and concluded "the pair is atomic" — true of the *out-source*
control, false of the sampling page, which that capture never exercised.
Because bit1 is what actually routes Main Out to USB-C, a sampling-source
change silently reverted USB-C out to Mic while `37 14` still read Main Out, so
nothing re-asserted. That is the **in-session** half of "sometimes reverts to
the microphone"; the boot gate below is the across-reboot half.

`xmos_audio_state_t.monitor` therefore tracks bit1 in its own right (it is
deliberately NOT folded into `scan`'s `changed` return — that flag means "the
out-source selection moved", and this is not a selection), and
`usbc_gate_tick_monitor` re-asserts on `usbc_out == 1 && monitor == 0`. The
`37 14` half of that test is what keeps it off a deliberate Mic selection,
which moves both. **The debounce is load-bearing**: the pair can split across
SPI frames (16 of 20 MIDI_OUT slots), so acting on a single tick would fight
the leading half of a split Mic selection. Two consecutive worker ticks
(~400 ms) against ~3 ms frames settles that. Verified on hardware — Move's
`37 12 01` at f75529, our `37 12 03` at f75635 (**bit0 preserved, bit1
restored**), then quiet.

Flow: the SPI pre-transfer callback scans MIDI_OUT via `xmos_audio_scan`; the
worker persists the value to `/data/UserData/schwung/usbc_out_state`; ~5 s after
boot the worker arms a replay, which the SPI callback emits one message per
frame. Only Main Out is replayed — Mic is Move's own boot default, so there is
nothing to correct and nothing goes on the wire.

Two behaviours worth knowing:

- **Persistence is gated CAUSALLY, not on a deadline** (`src/host/usbc_out_gate.c`).
  Move asserts its Mic default at ~0.6 s, and the shim observes its *own* replay
  too (emit runs earlier in the same `pre_transfer` than scan) — neither carries
  user intent and persisting either clobbers the stored preference.

  This was a ~7 s deadline, and **that deadline was a bug**. The worker's clock
  starts when MoveOriginal opens the SPI device; Move's assert floats with boot
  load. A slow boot put the assert on the trusting side of the line, so Mic was
  written over a stored Main Out — reverting in session *and* forgetting across
  the reboot, one mechanism producing both halves of the symptom, intermittent
  by construction. Confirmed on hardware: one boot logging `USB-C out: boot
  re-assert Main Out` and the state file later reading `0` with no user action.

  The discriminator is not time. **We only ever re-assert Main Out, so during
  the boot window an observed Mic can only have come from Move.** The gate
  stays closed until Move has had its say *and* we have re-asserted over it —
  pre-replay observations are recorded but never persisted; while defending, an
  observed Mic is countered (bounded by `USBC_GATE_MAX_REPLAYS`) rather than
  believed; an observed Main Out only settles the gate once Move has actually
  asserted Mic this boot, so our own echo cannot settle it early on a slow boot.
  A ~60 s `usbc_gate_force_settle` backstop covers a boot where Move never
  speaks (opening the gate persists nothing by itself). Trade-off, unchanged in
  kind but now bounded by events: a change made before the ~5 s re-assert is not
  persisted. Unit tests: `tests/host/test_usbc_out_gate.sh`.
- **Move's own Settings screen keeps reading "Mic"** even when the hardware is on
  Main Out — Move doesn't adopt the replayed value into its UI state. The audio
  is correct; the screen is not. Selecting "Main Out" there is harmless;
  selecting "Mic" (believing it a no-op) actually switches it off.

**Global Settings → Audio → USB-C Persist** (`usbc_out_persist`, default On)
governs *whether Schwung restores* the value — deliberately **not** a second
Mic/Main Out picker, which could disagree with Move's. Its value column
annotates the source last seen on the wire (`On (Main Out)`), which is the only
honest read given Move's screen goes stale. Params: `master_fx:usbc_out_persist`
(get/set) and `master_fx:usbc_out_source` (get only; -1 unknown, 0 Mic, 1 Main
Out). Persisted to `shadow_config.json`, which the **shim parses at init**
(`native_resample_bridge_load_mode_from_shadow_config`) — so the flag is known
before the ~5 s replay and the restore needs no runtime propagation.

Impl: `src/host/shadow_xmos_audio.c` (pure codec — no I/O, allocation or locks,
so it is both SPI-callback-safe and host-testable; unit tests in
`tests/host/test_xmos_audio.sh`), observed and emitted in `schwung_shim.c`'s
pre-transfer callback, persisted and armed in `src/host/shim_worker.c`. The
boot arbitration is split out as `src/host/usbc_out_gate.c` — also pure state,
with no clock of its own, which is what makes the boot orderings testable
without a device.

`xmos_audio_emit` is also the only sanctioned way to put SysEx into MIDI_OUT: it
requires a **contiguous** run of free slots, refuses while any cable-0 SysEx is
mid-flight, and never partial-writes. The `spi_sysex_inject` debug trigger was
rerouted through it — the old path blind-wrote `out[0..31]` regardless of what
Move had queued, and a stuck injection like that hard-powered-off the device
twice.

