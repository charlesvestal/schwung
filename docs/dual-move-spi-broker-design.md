# SPI broker for dual MoveOriginal — design sketch

This is the implementation-ready design for extending `src/schwung_shim.c`
so two `MoveOriginal` processes can share `/dev/ablspi0.0`. Read
`docs/dual-move-progress.md` first for context on what's already proven.

## Goal

Two `MoveOriginal` processes run simultaneously. Audio from both is
mixed into the device's DAC. Display, LEDs, and MIDI-in are routed to
the *focused* instance only; a hardware shortcut flips focus.

## Constraints

- SPI callback is SCHED_FIFO 90 on core 3, ~900µs budget per frame
  (44.1 kHz / 128 frames = ~2.9ms total period; ~2ms is hardware
  transfer; the rest is ours). **No file I/O, no allocations, no locks
  the non-RT side holds.** Per `docs/REALTIME_SAFETY.md`.
- The shim is loaded into MoveOriginal via LD_PRELOAD. Each MoveOriginal
  has its own copy of the shim in its address space. Cross-process state
  must live in SHM with lock-free seq counters, not in process memory.
- `/dev/ablspi0.0` may be opened by exactly one process. Stock
  MoveOriginal is the one that opens it today; the shim hooks `ioctl`
  on it. Instance B's shim must NOT open the device — it talks to
  instance A's shim via SHM instead.

## Architecture

```
                  ┌─────────────────────────────┐
                  │   Move hardware MCU         │
                  │   /dev/ablspi0.0            │
                  └─────────────┬───────────────┘
                                │  SPI ioctl
                                │
                  ┌─────────────▼───────────────┐
                  │ MoveOriginal A (stock)      │
                  │   schwung-shim.so           │
                  │   - opens /dev/ablspi0.0    │
                  │   - SPI broker (this work)  │
                  │   - existing shadow path    │
                  └─────────────┬───────────────┘
                                │
                                │  SHM rendezvous
                                │  /schwung-move-b-tx (768B mailbox)
                                │  /schwung-move-b-rx (RX echo back)
                                │
                  ┌─────────────▼───────────────┐
                  │ MoveOriginal B (private bus)│
                  │   schwung-shim.so (B mode)  │
                  │   - never opens real SPI    │
                  │   - intercepts ioctl, reads │
                  │     SHM-A → fakes to caller │
                  │   - writes its TX to SHM-B  │
                  └─────────────────────────────┘
```

Instance A is the **broker**. Instance B is a **client** that thinks
it owns SPI but actually round-trips through SHM to A.

## Identifying which instance is which

The shim runs the same code in both processes. It needs to decide its
role at init.

**Detection logic** (in `mux_init`-style constructor):

1. If `MOVE_INSTANCE_ROLE=client` set in env → B mode (client).
2. Else if `/schwung-control` says an A is already running → also B.
3. Else → A mode (broker).

Default-to-A means stock MoveOriginal (which Schwung's installer
configures with `LD_PRELOAD=schwung-shim.so` but no role env) becomes
the broker. Instance B is launched with `MOVE_INSTANCE_ROLE=client` set
explicitly by `scripts/dual-move-launch.sh`.

## SHM layout (new)

```c
// src/host/shadow_constants.h — additions
#define SCHWUNG_MOVE_B_TX_SHM "/schwung-move-b-tx"
#define SCHWUNG_MOVE_B_RX_SHM "/schwung-move-b-rx"

typedef struct {
    uint32_t seq;            // monotonic, written-after-payload
    uint8_t  payload[768];   // matches SPI TX layout; see docs/SPI_PROTOCOL.md
    uint32_t pad;            // align to 8
} schwung_move_tx_shm_t;

typedef struct {
    uint32_t seq;
    uint8_t  payload[768];   // matches SPI RX layout
    uint32_t pad;
} schwung_move_rx_shm_t;
```

Each instance writes its seq counter AFTER the payload (release-store
semantics) so the reader sees consistent buffers without locks.

## A-mode (broker) ioctl path

Hook `ioctl(fd, SPI_IOC_MESSAGE...)`:

1. Run the existing pre-transfer logic (shadow audio mix, etc.).
2. **NEW:** read `/schwung-move-b-tx` SHM:
   - Audio (offset 256, 512 bytes): sum into the mailbox.
   - MIDI_OUT (offset 0, 80 bytes): merge LED/display from B *only if*
     `active_move_instance == B`. Otherwise drop B's MIDI_OUT.
   - Display (offset 80, 176 bytes): same — only if focused.
3. Run the existing real `ioctl` against `/dev/ablspi0.0`.
4. Run the existing post-transfer logic.
5. **NEW:** write `/schwung-move-b-rx` SHM with the response:
   - MIDI_IN (offset 2048, 248 bytes): empty if `active_move_instance ==
     A` (don't deliver pads/knobs to B). Pass through if B is focused.
   - Audio_IN (offset 2304, 512 bytes): always pass through to B (line
     in is non-exclusive).

## B-mode (client) ioctl path

Hook `ioctl(fd, SPI_IOC_MESSAGE...)`:

1. Receive call from MoveOriginal-B with its TX buffer at `xfer->tx_buf`.
2. Copy into `/schwung-move-b-tx.payload`, bump seq.
3. Block-wait on `/schwung-move-b-rx.seq` to advance (poll with short
   sleep — RT budget on B's thread is the same as A's, so this needs
   a futex or eventfd, not a busy-loop).
4. Copy `/schwung-move-b-rx.payload` into `xfer->rx_buf`.
5. Return success — MoveOriginal thinks the SPI transfer happened.

The "fake fd" for `/dev/ablspi0.0` in B-mode: open `/dev/null` or a
memfd at constructor time, return that fd to any `open("/dev/ablspi0.0",
...)` call. B's shim intercepts the open too.

**Synchronization:** B's call period is independent of A's (A drives the
real SPI cadence, ~44Hz). B might queue 0–2 frames behind. That's fine
audio-wise; mailbox-summing handles it. Display/LED gating by focus
flag handles itself per-frame.

## Focus flag

```c
// src/host/shadow_constants.h — additions
typedef enum {
    ACTIVE_MOVE_A = 0,
    ACTIVE_MOVE_B = 1,
} active_move_t;

// existing shadow_control_t — add:
//   uint8_t active_move_instance;   // 0=A, 1=B
//   uint8_t pad[3];
```

A-mode broker reads `shadow_control->active_move_instance` once per
frame and uses it to decide display/LED/MIDI-in routing.

## Focus-switch shortcut

Recon doc proposes **Shift+Vol+Step1** (Step 2 = Global Settings, Step
13 = Tools; Step 1 unused).

In `src/schwung_shim.c` the existing shortcut detection (search
`shadow_ui_trigger`, `Shift+Vol+`) handles Shift+Vol combos already.
Extend to detect Step 1 and:

1. Toggle `shadow_control->active_move_instance ^= 1`.
2. Mark the new active instance's display dirty (so it does a full
   redraw on the next frame — A keeps its display state in process
   memory; B's is in process B's memory; we don't have to do anything
   special here, the new active instance's next ioctl will write a fresh
   display chunk).
3. Replay the new active instance's last-known LED state — cache the
   most recent LED packets per instance in the broker so we can flush
   them on switch (single write of ~64 LEDs). Without this, the LEDs
   stay stuck at the previous instance's state until that instance
   sends new ones.

## Audio mixing

The existing shadow path already sums shadow audio with Move's audio
(`mailbox = mv·move + me_bus·mv` per `CLAUDE.md`'s gain staging notes).
Extension is a single 128-frame stereo int16 add for B's audio:

```c
// in pre-transfer, after existing shadow mix:
int16_t *mb = (int16_t *)(mailbox + 256);    // 128 stereo frames
int16_t *bb = (int16_t *)(b_tx + 256);
for (int i = 0; i < 256; ++i) {              // 128*2
    int32_t s = mb[i] + bb[i];
    mb[i] = (s > 32767) ? 32767 : (s < -32768) ? -32768 : s;
}
```

Saturating add. ~1µs on this CPU.

Optional refinement: per-instance volume in `/schwung-control` so
unfocused instance can be quieter (or muted). Not in v1.

## Things to test

1. Boot both instances cold. Both reach a "running" state.
2. Both produce audio simultaneously without underruns over 5 minutes.
3. Focus switch is clean: display flips within one frame, LEDs match
   the new active instance, MIDI-in (pads / knobs) routes to the new
   instance.
4. Killing instance B doesn't crash A. Vice versa.
5. Stock-only mode (B never spawned) is byte-identical to today's
   behavior — the broker code path must be a no-op when no B is
   present (check `/schwung-move-b-tx.seq` hasn't advanced for ≥2
   frames → treat as no client).

## Things deliberately NOT in scope for v1

- Per-instance Schwung shadow features (only A gets shadow chains).
  Instance B runs raw libc audio engine, no shadow access.
- Per-instance MoveWebService / SystemDBusService / UpdateDBusService.
  Instance B's bus has no peer services; it logs ServiceUnknown for
  hostname / connman / update calls and continues. This is observed-
  working behavior.
- Per-instance UserData. Both share `/data/UserData/`. UUIDs prevent
  Set-file collisions; `Settings.json` torn-write is accepted.
- "Sleep" / suspend the unfocused instance. It runs full-rate, just
  with its display/LEDs/MIDI-in dropped on the floor by the broker.

## File-by-file change list

```
src/host/shadow_constants.h
  + SCHWUNG_MOVE_B_TX_SHM, SCHWUNG_MOVE_B_RX_SHM defines
  + schwung_move_tx_shm_t, schwung_move_rx_shm_t structs
  + active_move_t enum
  + shadow_control_t::active_move_instance field

src/schwung_shim.c
  + role detection in constructor (A vs B)
  + B-mode: open() hook for /dev/ablspi0.0 returns memfd
  + B-mode: ioctl(SPI_IOC_MESSAGE) → SHM round-trip
  + A-mode: read move-b-tx SHM in pre-transfer, mix audio + gate display/LED
  + A-mode: write move-b-rx SHM in post-transfer, gate MIDI-in
  + A-mode: focus-switch shortcut detection (Shift+Vol+Step1)
  + A-mode: per-instance LED state cache, replay on switch

scripts/dual-move-launch.sh
  + add MOVE_INSTANCE_ROLE=client to instance-B env

dual-move/instance-b.conf
  (no change)

docs/dual-move-progress.md
  (update with results of v1 implementation)
```

## Estimated scope

- ~300 LOC added to `src/schwung_shim.c` (mostly straight-line code, a
  few pointer copies and a saturating-add inner loop).
- ~30 LOC added to `src/host/shadow_constants.h`.
- ~10 LOC tweaks to `scripts/dual-move-launch.sh`.

Plus testing iteration on hardware, which is where the real time goes.

## Open question

The mysterious "FileExists" D-Bus error from the original PoC log only
fires when stock A is alive at the same time as instance B (regardless
of bus configuration). Once the SPI broker is in place, stock A and
instance B run simultaneously by design — so this error WILL fire on
every dual-instance boot. Investigate: is it actually fatal in the
running-broker scenario? In the PoC it happened ~35ms before the SPI
exit, so maybe MoveOriginal continued past it and only died on SPI; if
SPI succeeds, maybe the FileExists is just noise. Worth a 30-min
verification once the broker is up.
