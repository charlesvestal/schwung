# USB-C multi-out: Move as a 10-in audio interface

Status: modules built and hardware-verified; gadget brought up by hand; the
audio feed is not written yet. All work is on `worktree-usbc-uac2-multiout`
(PR #416) and nothing is persisted on the device.

## What this is

Move appears to a connected computer as a **10-input USB audio interface** —
the four chain slots plus Move's own mix, as separate inputs in a DAW. Not
network audio: a class-compliant USB device, no driver.

## Why the obvious route is closed

The USB-C audio device a Mac sees **today** is the XMOS. Its firmware is closed
and absent from Ableton's GPL drop, and the Pi could not feed it more channels
anyway — SPI carries exactly 128 stereo int16 each way, and there is no third
channel pair in the layout to widen into. Reflashing XMOS is not the route.

## The route that works

`drivers/usb/gadget/function/f_uac2.c` **is** in the GPL source; it is simply
not compiled, because `CONFIG_SND` is unset. Three config facts make shipping it
as loadable modules realistic:

- `CONFIG_SOUND=y` — soundcore is already in the running vmlinux, so only
  `snd`/`snd-pcm` are missing and both are ordinary modules.
- `# CONFIG_MODULE_SIG is not set` — unsigned modules load.
- `CONFIG_MODVERSIONS=y` — symbol CRCs must match, which is what the build gate
  checks.

**No kernel image is replaced.** `scripts/build-uac2-modules.sh` builds the
kernel twice (stock, then with `SND=m SND_PCM=m USB_CONFIGFS_F_UAC2=y`) and
refuses to package unless the ABI held. Measured: 10231 -> 10405 exported
symbols, **0 changed, 0 dropped**, 368 module dependencies all satisfied, and
build 2 recompiles only module objects — it never relinks vmlinux.

Five modules, not four: `snd-pcm` pulls in `snd-timer`.

The CRC comparison lives in `scripts/verify-uac2-abi.sh` so
`tests/host/test_uac2_abi_gate.sh` can prove it rejects a moved CRC, a dropped
symbol, and both — a gate that cannot fail is not a gate.

## Hardware facts

```
dwc2 fe980000.usb: EPs: 8, dedicated fifos, 4080 entries in SPRAM
UDC maximum_speed = high-speed
```

- **Endpoints:** NCM uses 2 IN + 1 OUT; a playback-only UAC2 adds 1 iso IN.
  ~3 of 7. Fine.
- **FIFO:** 4080 DWORDs minus 2048 (rx) and 1024 (np_tx) leaves ~1008 averaged
  across IN endpoints (`dwc2_set_param_tx_fifo_sizes` does this automatically,
  so the classic "not enough SPRAM" pain does not apply). A 10ch/44.1k packet
  at high speed is **140 bytes** against a 1024 cap; the ceiling is ~36ch at
  32-bit.
- **dwc2 gadget isochronous is supported** on both the DDMA and buffer-DMA
  paths. The only restrictions — ISOC IN DDMA rejects `bInterval > 10`, HB ISOC
  OUT DDMA unsupported — do not bind us.
- **Root on the device is `ssh root@move.local`** (key auth). There is no
  `sudo` binary and `ableton` cannot load modules.

## The composite device needs IAD, and it does not break NCM

The stock gadget sets `bDeviceClass = 2` (Communications), correct for a lone
NCM function. A composite NCM + UAC2 device must declare an Interface
Association Descriptor — `0xEF / 0x02 / 0x01` — or a host binds only the first
function. This is the one change with a blast radius outside the feature, since
it alters how existing hosts enumerate the **ethernet** gadget, and there is a
`WINNCM` OS descriptor in `/etc/init.d/setup-usb-network-gadget` for Windows.

Verified with both functions bound: UDC `configured` at `high-speed`, `usb0`
`LOWER_UP`, the Mac took a DHCP lease, and schwung-manager answered over the
cable (`HTTP 303 in 4 ms` via `172.16.254.1`). **Not yet tested on Windows.**

## Verified end to end

The device streamed 10 s of tones (channel N at `(N+1)*100` Hz) while the Mac
captured all ten channels via `ffmpeg -f avfoundation -i ":5"`. FFT of a 4 s
window put every channel's peak within 1.5 Hz of its own tone, identical
per-channel RMS, no cross-talk, **zero xruns**. `tools/uac2/uac2_tone.c`.

Two things that look like bugs and are not:

- **`-EIO` from `snd_pcm_writei` when the host is not streaming.** Nothing
  drains the PCM, the buffer fills, the write fails. It goes away the moment
  something opens the input. Do not debug it.
- **macOS names the device "Capture Inactive"** (avfoundation index 5; index 6
  is the XMOS's "Ableton Move Audio"). Cosmetic, from the descriptor strings.

## Channel map

**The channel count is fixed at enumeration**, because `p_chmask` is baked into
the descriptors the host reads when it binds. Changing 10 -> 8 on a mode toggle
would need a UDC unbind/rebind: the audio device vanishes mid-session, every
DAW holding that input drops it, and `usb0` goes with it. So the count is
**always 10** and only the content follows the mode — the same way Save Stems
varies its content while its file layout stays put.

Stems are **pre-Master-FX and pre-master-volume**, so a DAW gets unity audio
that the volume knob does not touch.

| ch | Move->Schwung ON | Move->Schwung OFF |
|----|------------------|-------------------|
| 1-8 | slot N = `move_track + synth`, slot FX on the **sum**, at slot volume | slot N = Schwung's synth through its FX only |
| 9-10 | **silent** — Move's audio is already inside ch1-8 | Move's mailbox mix, un-scaled back to unity |

- Under Move->Schwung a slot with **no module loaded still carries its Move
  track** (`schwung_shim.c:2656`), so an empty slot is not a dead channel.
- The Move pair is silent under Move->Schwung **by construction**, not by
  omission: a sixth source repeating them would double every instrument.
- An idle slot sets `shadow_stem_valid[s] = 0`, which the consumer writes as
  **silence, never stale audio**. The stem bus is explicitly sample-aligned
  across all five, which is what a multitrack DAW needs.

Two silent channels read as a broken interface. UAC2 carries per-channel names,
so they should enumerate as `Slot 1 L/R … Move L/R` — the same string table
that currently makes macOS say "Capture Inactive", so both are one fix.

## The feed (not yet written)

### Tap the stem bus, not `/schwung-pub-audio`

`/schwung-pub-audio` already publishes per-slot audio continuously and looks
like the obvious source. It is the wrong one: its 5th slot is the **ME master,
not the Move stem**, several write sites are gated on `link_audio.enabled`, and
an idle slot **stops advancing `write_pos`** rather than writing silence — so
channels would drift out of sample alignment against each other.

The source is `shadow_stem_bus[5][FRAMES_PER_BLOCK*2]` with
`shadow_stem_valid[5]`, in `shadow_inprocess_mix_from_buffer()`
(`schwung_shim.c:2234`), handed off by `shadow_stem_dispatch()`.

### The Save Stems gate has to widen

`shadow_stems_wanted` is driven by the Save Stems **setting**, and
`shadow_stem_dispatch` is additionally inside
`if (sampler_source == SAMPLER_SOURCE_RESAMPLE)`. Tapping there unchanged means
USB multi-out silently works only when Save Stems happens to be on.

The wants bit becomes *Save Stems is on* **or** *the USB host is streaming*.
This is a feature, not a workaround: with nothing capturing, the stem path stays
switched off and costs exactly what it costs today.

### There is no published `rebuild_from_la`

It is recomputed per frame from four terms at `schwung_shim.c:2346`. A consumer
that re-derives the predicate will get it subtly wrong (there are already two
near-copies in the file, `any_la_rebuild` and `skip_deferred_fx`, each missing a
different term). Write a latch where it is computed and publish that.

### Shape

```
SPI callback  ->  shadow_stem_bus[5]  ->  /schwung-uac2-out ring
                                              |
                              uac2-bridge (SCHED_OTHER, cores 0-2)
                                              |
                                    u_audio PCM (hw:0,0)  ->  USB iso IN  ->  host
```

The bridge must not run at the callback's priority — `pthread_create` from
there inherits **FIFO 70**, which starves Move's own `Link Main` at 35.

Open question: the XMOS is clock master and the iso IN endpoint is
asynchronous, so the host adapts — but the bridge still has to handle the
gadget PCM draining at the host's rate against the SPI frame's rate. Drift is
the part most likely to bite.

## Remaining work

1. The SHM ring + shim publish (widen the wants gate, add the rebuild latch).
2. The bridge process.
3. Per-channel descriptor names, and a device name that is not "Capture Inactive".
4. Persist the gadget: `scripts/uac2-gadget.sh` is runtime-only today; folding
   it into `/etc/init.d/setup-usb-network-gadget` and `install.sh` is a separate
   step, and wants a settings toggle.
5. Windows enumeration test for the IAD change.
6. **Maintenance tax:** a firmware update wipes the modules; a kernel version
   bump breaks vermagic and forces a rebuild. `install.sh` must detect and
   re-place them.
